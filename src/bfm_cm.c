/* 接続の確立と切断(bfmem どうし。GSI = QP1 の上で、bfmem だけの短いメッセージを交わす)。
 *
 * RC QP を繋ぐには、相手の QP 番号と開始 PSN、公開メモリのアドレスと rkey を
 * 交換する必要がある。IP のスタックを持たないので、交換は RoCE v2 の UD で、相手の
 * QP1(番号が決まっている唯一の QP)へ送る。相手は常に bfmem なので、IB CM ではなく
 * 次の 3 種類だけを使う。
 *
 *   掛け側 bfm_connect(): REQ ->(REP が来るまで送り直す) <- REP
 *   受け側 bfm_accept():  <- REQ  QP を RTS にしてから REP ->
 *   切断   bfm_disconnect(): BYE ->(返事は待たない。相手は bfm_poll() で受けて閉じた扱いにする)
 *
 * 受け側は REP を送る前に RTS にするので、掛け側は REP を受けて RTS にすればすぐ使える
 * (RTU は要らない)。REP が落ちたときは掛け側が REQ を送り直し、受け側は確立済みの接続への
 * REQ に REP を送り直す。
 *
 * メッセージ(MSG_SIZE バイト、数はビッグエンディアン):
 *   [0..3] 'BFM2'  [4] 種類  [5] 経路 MTU  [6..7] 待ち受けの番号
 *   [8..11] 送り手の接続の id  [12..15] 受け手の接続の id(REQ では 0)
 *   [16..19] QPN  [20..23] 開始 PSN  [24] 受け付ける read / atomic の数
 *   [26..31] 送り手の MAC  [32..35] 送り手の IPv4
 *   [36..39] 公開メモリの rkey  [40..47] 公開メモリの先頭  [48..55] 公開メモリの大きさ
 *   [56..59] 宛先の IPv4
 *
 * ARP を使わない。掛け側が相手の MAC を知らなければ(引数も g_bfm_peer_mac も全部 0)、
 * REQ を同報(宛先の MAC が ff:ff:ff:ff:ff:ff)で送り、REP に入った相手の MAC を使う。
 * 同報は宛先でない bfmem にも届くので、受け手は [56..59] が自分の IPv4 でないものを捨てる。
 *
 * 受け取ったメッセージは gsi_pump() がその場で処理する(REP はその接続の枠へ写し、BYE は
 * 接続を閉じた扱いにし、REQ は bfm_accept() のために接続の枠の数まで取っておく)。
 *
 * 接続の確立と切断は、接続の枠ごとの状態機械(conn_step())で進める。待つところ(FW の
 * コマンドの完了・REQ / REP の到着・GSI の送信の空き)では待たずに BFM_INPROGRESS を返し、
 * bfm_connect() / bfm_accept() / bfm_disconnect() を呼び直すたびに先へ進む。
 * 複数のコアから呼べるように、GSI・接続の枠の取り外し・FW のコマンドは g_bfm_ctl_lock の中で行う
 * (1 回の呼び出しの間だけ持つ)。 */
#include "bfm_internal.h"

#include <string.h>

#define MSG_MAGIC         0x42464d32u /* 'BFM2' */
#define MSG_SIZE          64u
#define MSG_REQ           1u
#define MSG_REP           2u
#define MSG_BYE           3u

#define REQ_RETRY_MS      1000u       /* REP が来なければ REQ を送り直す間隔 */
#define REQ_TIMEOUT_MS    5000u       /* bfm_connect() の timeout_ms = 0 のときの上限 */
#define REQ_STALE_MS      2000u       /* これより古い REQ は掛け側があきらめたとみなして捨てる */
#define TX_DRAIN_MS       100u        /* BYE を出す・出ていくのを待つ上限 */

/* 接続の枠の状態機械の手順(struct bfm_conn の step)。 */
enum {
    K_IDLE = 0,       /* 使っていない(state = BFM_CS_FREE)*/
    K_WAIT_REQ,       /* 受け側: REQ を待つ */
    K_CQ,             /* CREATE_CQ */
    K_QP,             /* CREATE_QP */
    K_INIT,           /* RST2INIT_QP */
    K_WAIT_REP,       /* 掛け側: REQ を送り(送り直し)、REP を待つ */
    K_RTR,            /* INIT2RTR_QP */
    K_RTS,            /* RTR2RTS_QP(atomic_mode が断られたら付けずにもう一度)*/
    K_REP,            /* 受け側: REP を送る */
    K_UP,             /* 使える */
    K_BYE,            /* 畳む: 相手へ BYE を送る */
    K_DESTROY         /* 畳む: QP と CQ を消して枠を空ける */
};
enum { ROLE_CONNECT = 1, ROLE_ACCEPT = 2 };

struct bfm_conn g_bfm_conn[BFM_MAX_CONN];   /* 接続の枠(使うのは先頭の g_bfm.nconn 個)*/

static bfm_qp_t s_gsi;                      /* 接続管理(REQ / REP / BYE)を運ぶ GSI の QP(QP1)*/
static volatile uint8_t *s_gsi_db;          /* GSI の UAR + 0x800(UC)*/
static volatile uint8_t *s_rx_ring;         /* 受信の枠(2^BFM_GSI_RQ_LOG 個)*/
static volatile uint8_t *s_tx_buf;          /* 送るメッセージの置き場(BFM_GSI_TXBUFS 個)*/
static uint32_t s_tx_next;                  /* 次に使う送信の置き場の通し番号(BFM_GSI_TXBUFS で割った余りが位置)*/
static uint32_t s_tx_outstanding;           /* 出して、まだ完了していない送信の数 */
static int s_gsi_ready;                     /* GSI の QP と置き場がそろい、接続管理を使える */

/* bfm_accept() を待っている REQ(接続の枠の数まで。同じ掛け側からの送り直しは 1 つにまとめる。
 * 一杯なら最も古いものを上書きする。掛け側が送り直すので、落としてもよい)。 */
#define REQ_Q BFM_MAX_CONN
static struct {
    int      used;                          /* 中身がある */
    uint64_t t;                             /* 受けた時刻(ns。古さを見る)*/
    uint8_t  msg[MSG_SIZE];                 /* 受けた REQ のメッセージそのもの */
} s_req[REQ_Q];                             /* 受けて、まだ bfm_accept() に渡していない REQ */

static int gsi_pump(void);

static uint32_t cm_rand32(void)
{
    static uint64_t x;
    x ^= bfm_port_time_ns() + 0x9e3779b97f4a7c15ull;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (uint32_t)(x >> 16) | 1u;                /* 0 は「まだ無い」の意味に使うので避ける */
}

static uint64_t ms_since(uint64_t t0)
{
    return (bfm_port_time_ns() - t0) / 1000000u;
}

/* MAC が全部 0(知らない)か。 */
static int mac_is_zero(const uint8_t mac[6])
{
    for (unsigned i = 0; i < 6u; i++) {
        if (mac[i] != 0u) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------
 * メッセージ
 * ------------------------------------------------------------------- */

static uint8_t own_rr(void)
{
    const uint8_t l = (g_bfm.log_max_ra_res_qp > 4u) ? 4u : g_bfm.log_max_ra_res_qp;
    return (uint8_t)(1u << l);
}

/* 自分の情報を詰めたメッセージを作る(REQ / REP)。BYE は id までだけ。 */
static void build_msg(uint8_t *m, uint8_t type, const struct bfm_conn *c)
{
    memset(m, 0, MSG_SIZE);
    bfm_wr32be(m, MSG_MAGIC);
    m[4] = type;
    m[5] = c->path_mtu;
    bfm_wr16be(m + 6, c->port);
    bfm_wr32be(m + 8, c->local_id);
    bfm_wr32be(m + 12, c->peer_id);
    bfm_wr32be(m + 56, c->peer_ip);
    if (type == MSG_BYE) return;
    bfm_wr32be(m + 16, c->qp.qpn);
    bfm_wr32be(m + 20, c->qp.local_psn);
    m[24] = own_rr();
    memcpy(m + 26, g_bfm.mac, 6);
    bfm_wr32be(m + 32, g_bfm.ip);
    bfm_wr32be(m + 36, g_bfm.rkey);
    bfm_wr64be(m + 40, g_bfm_mr_base);
    bfm_wr64be(m + 48, g_bfm_mr_size);
}

/* REQ / REP から相手の情報を接続の枠へ写す。 */
static void take_peer(struct bfm_conn *c, const uint8_t *m)
{
    c->peer_id = bfm_rd32be(m + 8);
    c->peer_qpn = bfm_rd32be(m + 16) & 0xffffffu;
    c->peer_psn = bfm_rd32be(m + 20) & 0xffffffu;
    c->peer_rr = m[24] ? m[24] : 1u;
    memcpy(c->peer_mac, m + 26, 6);
    c->peer_ip = bfm_rd32be(m + 32);
    mlx5_gid_from_ipv4(c->peer_ip, c->peer_gid);
    c->rkey = bfm_rd32be(m + 36);
    c->raddr = bfm_rd64be(m + 40);
    c->rlen = bfm_rd64be(m + 48);
}

/* ---------------------------------------------------------------------
 * GSI の送受信
 * ------------------------------------------------------------------- */

static void gsi_post_recv(void)
{
    const uint32_t n = 1u << s_gsi.rq_log;
    const uint32_t idx = s_gsi.rq_pc & (n - 1u);
    volatile uint8_t *w = s_gsi.wq + idx * 16u;
    bfm_wr32be(w, BFM_GSI_RX_SLOT);
    bfm_wr32be(w + 4, g_bfm.lkey);
    bfm_wr64be(w + 8, bfm_dma_addr(s_rx_ring + idx * BFM_GSI_RX_SLOT));
    s_gsi.rq_pc++;
    bfm_dma_wmb();
    s_gsi.dbr[0] = bfm_cpu_to_be32(s_gsi.rq_pc & 0xffffu);
}

/*=================================================================
 * メッセージ(MSG_SIZE バイト)を相手の QP1 へ送る。UD の WQE は
 * ctrl(16)+ datagram(48)+ data(16)で 2 WQEBB。送信の置き場が全部使われていれば、
 * GSI の CQ を 1 回読んでも空かなければ、待たずに BFM_INPROGRESS を返す(呼び出し側が後で送り直す)。
 * ===============================================================*/
static int gsi_send(const uint8_t *msg, const struct bfm_conn *c)
{
    if (s_tx_outstanding >= BFM_GSI_TXBUFS) {
        if (gsi_pump() < 0) {
            BFM_LOG("bfm: GSI のエラーで送れない\n");
            return BFM_E_CQE;
        }
        if (s_tx_outstanding >= BFM_GSI_TXBUFS) return BFM_INPROGRESS;
    }
    volatile uint8_t *buf = s_tx_buf + (s_tx_next++ % BFM_GSI_TXBUFS) * MSG_SIZE;
    for (unsigned i = 0; i < MSG_SIZE; i++) buf[i] = msg[i];

    const uint32_t n = 1u << s_gsi.sq_log;
    const uint32_t pc = s_gsi.sq_pc;
    volatile uint8_t *w0 = s_gsi.sq + (pc & (n - 1u)) * MLX5_SEND_WQE_BB;
    volatile uint8_t *w1 = s_gsi.sq + ((pc + 1u) & (n - 1u)) * MLX5_SEND_WQE_BB;
    for (unsigned i = 0; i < MLX5_SEND_WQE_BB; i++) {
        w0[i] = 0;
        w1[i] = 0;
    }
    bfm_wr32be(w0, ((pc & 0xffffu) << 8) | MLX5_OPCODE_SEND);
    bfm_wr32be(w0 + 4, (s_gsi.qpn << 8) | 5u);              /* ds = ctrl 1 + datagram 3 + data 1 */
    w0[11] = MLX5_WQE_CTRL_CQ_UPDATE;
    bfm_wr32be(w0 + 16, MLX5_QP1_QKEY);                      /* av.key.qkey */
    bfm_wr32be(w0 + 24, 0x80000000u | 1u);                   /* extended AV | 宛先 QPN = 1 */
    bfm_wr16be(w0 + 30, mlx5_udp_sport(s_gsi.qpn, 1u));
    for (unsigned i = 0; i < 6u; i++) w0[36 + i] = c->peer_mac[i];
    w0[43] = 64u;                                            /* hop_limit */
    bfm_wr32be(w0 + 44, 1u << 30);                           /* GRH あり、送り元の GID は 0 番 */
    for (unsigned i = 0; i < 16u; i++) w0[48 + i] = c->peer_gid[i];
    bfm_wr32be(w1, MSG_SIZE);
    bfm_wr32be(w1 + 4, g_bfm.lkey);
    bfm_wr64be(w1 + 8, bfm_dma_addr(buf));

    s_gsi.sq_pc = pc + 2u;
    s_tx_outstanding++;
    bfm_dma_wmb();
    s_gsi.dbr[1] = bfm_cpu_to_be32(s_gsi.sq_pc & 0xffffu);
    bfm_dma_wmb();
    uint64_t first8;
    memcpy(&first8, (const void *)(uintptr_t)w0, 8);
    bfm_mmio_wr64_raw(s_gsi_db, first8);
    bfm_wc_flush();
    return BFM_OK;
}

/* REQ を取っておく。同じ掛け側(接続の id と IPv4 が同じ)の送り直しなら置き換え、
 * そうでなければ空き(無ければ最も古いもの)に入れる。 */
static void req_put(const uint8_t *m)
{
    const uint32_t src = bfm_rd32be(m + 8), ip = bfm_rd32be(m + 32);
    unsigned k = 0;
    int found = 0;
    for (unsigned i = 0; i < REQ_Q; i++) {
        if (s_req[i].used && bfm_rd32be(s_req[i].msg + 8) == src && bfm_rd32be(s_req[i].msg + 32) == ip) {
            k = i;
            found = 1;
            break;
        }
    }
    for (unsigned i = 0; i < REQ_Q && !found; i++) {
        if (!s_req[i].used) {
            k = i;
            found = 1;
        } else if (s_req[i].t < s_req[k].t) {
            k = i;
        }
    }
    memcpy(s_req[k].msg, m, MSG_SIZE);
    s_req[k].t = bfm_port_time_ns();
    s_req[k].used = 1;
}

/* 待ち受けの番号 port 宛ての REQ のうち、最も古いものを取り出して msg に写す。
 * 古すぎる REQ(掛け側があきらめた)は捨てる。戻り値: 1 = 取り出した、0 = 無い。 */
static int req_take(uint16_t port, uint8_t *msg)
{
    int k = -1;
    for (unsigned i = 0; i < REQ_Q; i++) {
        if (!s_req[i].used) continue;
        if (ms_since(s_req[i].t) >= REQ_STALE_MS) {
            s_req[i].used = 0;
            continue;
        }
        if (bfm_rd16be(s_req[i].msg + 6) == port && (k < 0 || s_req[i].t < s_req[k].t)) k = (int)i;
    }
    if (k < 0) return 0;
    memcpy(msg, s_req[k].msg, MSG_SIZE);
    s_req[k].used = 0;
    return 1;
}

/* 受け取ったメッセージ 1 つを処理する。 */
static void handle_msg(const uint8_t *m)
{
    const uint8_t type = m[4];
    const uint32_t src = bfm_rd32be(m + 8), dst = bfm_rd32be(m + 12);
    if (bfm_rd32be(m + 56) != g_bfm.ip) return;          /* ほかの bfmem 宛て(同報の REQ)*/
    if (type == MSG_REQ) {
        /* もう受けた掛け側からの送り直し: 確立済みなら REP が落ちたので送り直し、
         * 用意の途中なら捨てる(REP は用意ができたら送る) */
        for (unsigned i = 0; i < g_bfm.nconn; i++) {
            struct bfm_conn *c = &g_bfm_conn[i];
            if (c->state != BFM_CS_FREE && c->peer_id == src && c->peer_ip == bfm_rd32be(m + 32)) {
                if (c->state == BFM_CS_UP && c->step == K_UP) {
                    uint8_t rep[MSG_SIZE];
                    build_msg(rep, MSG_REP, c);
                    (void)gsi_send(rep, c);
                }
                return;
            }
        }
        req_put(m);
        return;
    }
    for (unsigned i = 0; i < g_bfm.nconn; i++) {
        struct bfm_conn *c = &g_bfm_conn[i];
        if (c->local_id != dst) continue;
        if (type == MSG_REP && c->step == K_WAIT_REP && !c->got_rep) {
            take_peer(c, m);
            c->got_rep = 1;
        } else if (type == MSG_BYE && c->peer_id == src &&
                   (c->state == BFM_CS_UP || c->state == BFM_CS_ERROR)) {
            c->state = BFM_CS_CLOSED;
            BFM_LOG("bfm: 相手が接続 %d を切った(bfm_disconnect() で返すこと)\n", c->index);
        }
        return;
    }
}

/*=================================================================
 * GSI の CQ を読み切る。受信は handle_msg() で処理して受信を出し直し、送信の完了は
 * 数を減らすだけ。戻り値: 受け取ったメッセージの数(-1 = GSI のエラー)。
 * ===============================================================*/
static int gsi_pump(void)
{
    const uint32_t n = 1u << s_gsi.cq_log;
    int got = 0;
    for (;;) {
        volatile uint8_t *cqe = s_gsi.cq + (s_gsi.cq_cc & (n - 1u)) * 64u;
        const uint8_t op_own = cqe[63];
        if ((op_own & 1u) != ((s_gsi.cq_cc >> s_gsi.cq_log) & 1u) || (op_own >> 4) == MLX5_CQE_INVALID) break;
        bfm_rmb();
        const uint8_t opcode = (uint8_t)(op_own >> 4);
        const uint32_t byte_cnt = bfm_rd32be(cqe + 44);
        const uint16_t wqe_cnt = bfm_rd16be(cqe + 60);
        const uint8_t synd = cqe[55];
        s_gsi.cq_cc++;
        s_gsi.cq_dbr[0] = bfm_cpu_to_be32(s_gsi.cq_cc & 0xffffffu);
        if (opcode == MLX5_CQE_REQ) {
            if (s_tx_outstanding) s_tx_outstanding--;
        } else if (opcode == MLX5_CQE_RESP_SEND) {
            const uint32_t idx = wqe_cnt & ((1u << s_gsi.rq_log) - 1u);
            const volatile uint8_t *buf = s_rx_ring + idx * BFM_GSI_RX_SLOT + MLX5_GRH_BYTES;
            uint8_t m[MSG_SIZE];
            for (unsigned i = 0; i < MSG_SIZE; i++) m[i] = buf[i];
            gsi_post_recv();
            if (byte_cnt >= MLX5_GRH_BYTES + MSG_SIZE && bfm_rd32be(m) == MSG_MAGIC) {
                handle_msg(m);
                got++;
            }
        } else {
            BFM_LOG("bfm: GSI のエラー完了 (opcode 0x%x syndrome 0x%02x)\n", opcode, synd);
            if (opcode == MLX5_CQE_REQ_ERR && s_tx_outstanding) s_tx_outstanding--;
            if (opcode == MLX5_CQE_RESP_ERR) gsi_post_recv();
            return -1;
        }
    }
    return got;
}

/* ---------------------------------------------------------------------
 * 接続の枠の状態機械
 * ------------------------------------------------------------------- */

/* 空いている枠を取り、確立の手順の最初に置く(コマンドはまだ出さない)。 */
static struct bfm_conn *conn_open(uint16_t port, uint8_t role)
{
    struct bfm_conn *c = NULL;
    for (unsigned i = 0; i < g_bfm.nconn && c == NULL; i++) {
        if (g_bfm_conn[i].state == BFM_CS_FREE) c = &g_bfm_conn[i];
    }
    if (c == NULL) {
        BFM_LOG("bfm: 接続の枠が足りない(g_bfm_max_conn = %u)\n", g_bfm.nconn);
        return NULL;
    }
    c->state = BFM_CS_SETUP;
    c->role = role;
    c->step = (role == ROLE_CONNECT) ? K_CQ : K_WAIT_REQ;
    c->abort = 0;
    c->result = BFM_OK;
    c->port = port;
    c->local_id = cm_rand32();
    c->peer_id = 0;
    c->peer_ip = 0;
    c->got_rep = 0;
    c->path_mtu = (g_bfm_path_mtu >= 1u && g_bfm_path_mtu <= 5u) ? g_bfm_path_mtu : 3u;
    c->cs_req = g_bfm_scatter_cqe ? 0x11u : 0u;
    c->t0 = bfm_port_time_ns();
    c->t_sent = 0;
    memset(&c->cmd, 0, sizeof(c->cmd));
    return c;
}

/* 失敗した: 結果を覚えて畳む手順へ回す(次に conn_step() の頭で切り替わる)。 */
static void conn_fail(struct bfm_conn *c, int rc)
{
    c->result = rc;
    c->abort = 1;
}

/*=================================================================
 * 接続の枠 c の状態機械を、待つところまで進める。
 *   確立: [受け側 K_WAIT_REQ] -> K_CQ -> K_QP -> K_INIT -> [掛け側 K_WAIT_REP] -> K_RTR -> K_RTS
 *         -> [受け側 K_REP] -> K_UP
 *   畳む(c->abort): [UP / ERROR なら K_BYE] -> K_DESTROY -> 枠を空ける
 * 待つところ(コマンドの完了・REQ / REP・GSI の送信の空き)では BFM_INPROGRESS を返す。
 * 最初に EQ を読む(FW のページ要求に答える手順がコマンドの入口を持っていれば、それを進める)。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 使える(K_UP)。畳み終えたら c->result
 * (bfm_disconnect() なら BFM_OK、確立の失敗ならその理由)。g_bfm_ctl_lock の中で呼ぶ。
 * ===============================================================*/
static int conn_step(struct bfm_conn *c)
{
    int rc;
    /* FW のページ要求に答える手順がコマンドの入口を持っていることがあるので、ここでも進める */
    (void)mlx5_eq_poll(&g_bfm);
    for (;;) {
        /* 確立の時間切れ(掛け側。コマンドの途中でなければ)*/
        if (!c->abort && c->role == ROLE_CONNECT && c->state == BFM_CS_SETUP && !c->cmd.posted &&
            ms_since(c->t0) > c->timeout_ms) {
            BFM_LOG(c->step == K_WAIT_REP ? "bfm: 接続できない(REP が来ない)\n"
                                          : "bfm: 接続できない(時間内に用意が済まない)\n");
            conn_fail(c, BFM_E_TIMEOUT);
        }
        /* 畳む手順へ(コマンドを出していれば、その完了を受け取ってから)*/
        if (c->abort && c->step < K_BYE && !c->cmd.posted) {
            c->step = (c->state == BFM_CS_UP || c->state == BFM_CS_ERROR) ? K_BYE : K_DESTROY;
            c->t_sent = bfm_port_time_ns();
        }
        switch (c->step) {
        case K_WAIT_REQ: {
            uint8_t msg[MSG_SIZE];
            (void)gsi_pump();
            if (!req_take(c->port, msg)) {
                if (c->timeout_ms && ms_since(c->t0) > c->timeout_ms) {
                    conn_fail(c, BFM_E_TIMEOUT);
                    break;
                }
                return BFM_INPROGRESS;
            }
            take_peer(c, msg);
            if (msg[5] >= 1u && msg[5] < c->path_mtu) c->path_mtu = msg[5];
            c->step = K_CQ;
            break;
        }
        case K_CQ:
            rc = mlx5_cq_create(&g_bfm, &c->cmd, c->qp.uarn, c->qp.cq, c->qp.cq_log, c->qp.cq_dbr, &c->qp.cqn);
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) {
                conn_fail(c, rc);
                break;
            }
            c->qp.has_cq = 1;
            c->qp.cq_cc = 0;
            c->step = K_QP;
            break;
        case K_QP:
            rc = mlx5_qp_create(&g_bfm, &c->cmd, &c->qp, MLX5_QP_ST_RC, c->cs_req);
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) {
                conn_fail(c, rc);
                break;
            }
            c->step = K_INIT;
            break;
        case K_INIT:
            rc = mlx5_qp_rst2init(&g_bfm, &c->cmd, &c->qp, 0);
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) {
                conn_fail(c, rc);
                break;
            }
            c->step = (c->role == ROLE_CONNECT) ? K_WAIT_REP : K_RTR;
            break;
        case K_WAIT_REP:
            (void)gsi_pump();
            if (c->got_rep) {
                c->step = K_RTR;
                break;
            }
            if (c->t_sent == 0 || ms_since(c->t_sent) > REQ_RETRY_MS) {
                uint8_t req[MSG_SIZE];
                build_msg(req, MSG_REQ, c);
                rc = gsi_send(req, c);
                if (rc == BFM_OK) {
                    c->t_sent = bfm_port_time_ns();
                } else if (rc != BFM_INPROGRESS) {
                    conn_fail(c, rc);
                    break;
                }
            }
            return BFM_INPROGRESS;
        case K_RTR:
            rc = mlx5_qp_init2rtr_rc(&g_bfm, &c->cmd, &c->qp, c);
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) {
                conn_fail(c, rc);
                break;
            }
            c->amode = g_bfm.atomic_mode;
            c->step = K_RTS;
            break;
        case K_RTS:
            rc = mlx5_qp_rtr2rts_rc(&g_bfm, &c->cmd, &c->qp, c, c->amode);
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) {
                if (c->amode != 0u) {
                    c->amode = 0u;                  /* atomic_mode を付けずにもう一度 */
                } else {
                    conn_fail(c, rc);
                }
                break;
            }
            if (c->amode != g_bfm.atomic_mode)
                BFM_LOG("bfm: 注意: atomic_mode=%u は断られた。相手からの atomic は受けない\n", g_bfm.atomic_mode);
            if ((rc = bfm_ops_attach(c)) != BFM_OK) {
                conn_fail(c, rc);
                break;
            }
            c->state = BFM_CS_UP;
            BFM_LOG("bfm: 接続した(%s): qpn %u <-> %u  相手 %u.%u.%u.%u  公開メモリ 0x%llx+0x%llx rkey 0x%08x"
                    "  MTU %u  read/atomic %u 本  BlueFlame %s\n",
                    c->role == ROLE_CONNECT ? "掛け側" : "受け側", c->qp.qpn, c->peer_qpn,
                    c->peer_ip >> 24, (c->peer_ip >> 16) & 0xffu, (c->peer_ip >> 8) & 0xffu, c->peer_ip & 0xffu,
                    (unsigned long long)c->raddr, (unsigned long long)c->rlen, c->rkey, 128u << c->path_mtu,
                    c->max_rd, c->bf ? "あり" : "なし");
            c->step = (c->role == ROLE_ACCEPT) ? K_REP : K_UP;
            break;
        case K_REP: {
            uint8_t rep[MSG_SIZE];
            build_msg(rep, MSG_REP, c);
            rc = gsi_send(rep, c);
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) {
                conn_fail(c, rc);
                break;
            }
            c->step = K_UP;
            break;
        }
        case K_UP:
            return BFM_OK;
        case K_BYE: {
            /* 返事は待たない。出せなければ TX_DRAIN_MS まで出し直し、それでも出なければあきらめる */
            uint8_t bye[MSG_SIZE];
            build_msg(bye, MSG_BYE, c);
            rc = gsi_send(bye, c);
            if (rc == BFM_INPROGRESS && ms_since(c->t_sent) < TX_DRAIN_MS) return rc;
            c->step = K_DESTROY;
            break;
        }
        case K_DESTROY:
            bfm_ops_detach(c);
            if (mlx5_qp_destroy(&g_bfm, &c->cmd, &c->qp) == BFM_INPROGRESS) return BFM_INPROGRESS;
            c->state = BFM_CS_FREE;
            c->step = K_IDLE;
            c->role = 0;
            c->abort = 0;
            c->local_id = c->peer_id = 0;
            BFM_DBG("bfm: 接続 %d の枠を空けた\n", c->index);
            return c->result;
        default:   /* K_IDLE */
            return BFM_E_STATE;
        }
    }
}

/* ---------------------------------------------------------------------
 * 立ち上げと停止
 * ------------------------------------------------------------------- */

/* 立ち上げの状態(bfm_cm_init_step())。 */
enum { G_START = 0, G_UAR, G_CQ, G_QP, G_INIT, G_RTR, G_RTS, G_SLOT, G_DONE };

static struct {
    int step;                        /* 今の状態(G_*)*/
    unsigned i;                      /* 用意している接続の枠 */
    int slot_mem;                    /* 枠 i のメモリを切り出した */
    bfm_cmd_t cmd;                   /* 出しているコマンド */
} s_ci;

/* 停止の状態(bfm_cm_fini_step())。 */
enum { F_CONNS = 0, F_DRAIN, F_DONE };

static struct {
    int step;                        /* 今の状態(F_*)*/
    uint64_t t0;                     /* 送信が出ていくのを待ち始めた時刻 */
} s_cf;

/* 立ち上げ・停止の状態を最初に戻す(bfm_init() が始めるときに呼ぶ)。 */
void bfm_cm_reset(void)
{
    memset(&s_ci, 0, sizeof(s_ci));
    memset(&s_cf, 0, sizeof(s_cf));
    memset(&s_gsi, 0, sizeof(s_gsi));
    memset(&s_req, 0, sizeof(s_req));
    s_tx_next = s_tx_outstanding = 0;
    s_gsi_ready = 0;
}

/* 接続の枠 i のメモリ(WQ・CQ・ドアベルレコード・中継バッファ)を DMA 領域から切り出す。 */
static int slot_alloc(unsigned i)
{
    struct bfm_conn *c = &g_bfm_conn[i];
    memset(c, 0, sizeof(*c));
    c->index = (int)i;
    c->qp.rq_log = BFM_RC_RQ_LOG;
    c->qp.sq_log = BFM_RC_SQ_LOG;
    c->qp.cq_log = BFM_RC_CQ_LOG;
    c->qp.wq = (volatile uint8_t *)bfm_mem_alloc((16u << BFM_RC_RQ_LOG) + (MLX5_SEND_WQE_BB << BFM_RC_SQ_LOG), BFM_PAGE);
    c->qp.dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
    c->qp.cq = (volatile uint8_t *)bfm_mem_alloc(64u << BFM_RC_CQ_LOG, BFM_PAGE);
    c->qp.cq_dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
    c->stage_size = (g_bfm_stage_size + BFM_PAGE - 1u) & ~(BFM_PAGE - 1u);
    c->stage = (volatile uint8_t *)bfm_mem_alloc(c->stage_size, BFM_PAGE);
    c->dif_slot_size = (g_bfm_dif_buf_size / BFM_DIF_SLOTS + BFM_PAGE - 1u) & ~(BFM_PAGE - 1u);
    c->dif_buf = (volatile uint8_t *)bfm_mem_alloc((uint64_t)c->dif_slot_size * BFM_DIF_SLOTS, BFM_PAGE);
    if (!c->qp.wq || !c->qp.dbr || !c->qp.cq || !c->qp.cq_dbr || !c->stage || !c->dif_buf) return BFM_E_NOMEM;
    return BFM_OK;
}

/*=================================================================
 * GSI と接続の枠を、待つところまで用意する(bfm_init() から)。GSI の QP を RTS まで進めて
 * 受信を投稿し、接続の枠ごとにメモリと UAR を切り出す(UAR の WC の窓もここで張る)。
 * コマンドの完了を待つところで BFM_INPROGRESS を返す。失敗したときの WC の窓は
 * bfm_cm_fini_step() が返す。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。負 = 失敗。
 * ===============================================================*/
int bfm_cm_init_step(void)
{
    int rc;
    for (;;) {
        switch (s_ci.step) {
        case G_START:
            s_gsi.rq_log = BFM_GSI_RQ_LOG;
            s_gsi.sq_log = BFM_GSI_SQ_LOG;
            s_gsi.cq_log = BFM_GSI_CQ_LOG;
            s_gsi.wq = (volatile uint8_t *)bfm_mem_alloc((16u << BFM_GSI_RQ_LOG) + (MLX5_SEND_WQE_BB << BFM_GSI_SQ_LOG), BFM_PAGE);
            s_gsi.dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
            s_gsi.cq = (volatile uint8_t *)bfm_mem_alloc(64u << BFM_GSI_CQ_LOG, BFM_PAGE);
            s_gsi.cq_dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
            s_rx_ring = (volatile uint8_t *)bfm_mem_alloc((uint64_t)BFM_GSI_RX_SLOT << BFM_GSI_RQ_LOG, BFM_PAGE);
            s_tx_buf = (volatile uint8_t *)bfm_mem_alloc(BFM_GSI_TXBUFS * MSG_SIZE, BFM_PAGE);
            if (!s_gsi.wq || !s_gsi.dbr || !s_gsi.cq || !s_gsi.cq_dbr || !s_rx_ring || !s_tx_buf) return BFM_E_NOMEM;
            s_ci.step = G_UAR;
            break;
        case G_UAR:
            if ((rc = mlx5_alloc_uar(&g_bfm, &s_ci.cmd, &s_gsi.uarn)) != BFM_OK) return rc;
            s_gsi.has_uar = 1;
            s_gsi_db = g_bfm.bar0 + (uint64_t)s_gsi.uarn * BFM_PAGE + MLX5_BF_OFFSET;
            s_ci.step = G_CQ;
            break;
        case G_CQ:
            if ((rc = mlx5_cq_create(&g_bfm, &s_ci.cmd, s_gsi.uarn, s_gsi.cq, s_gsi.cq_log, s_gsi.cq_dbr,
                                     &s_gsi.cqn)) != BFM_OK) return rc;
            s_gsi.has_cq = 1;
            s_gsi.cq_cc = 0;
            s_ci.step = G_QP;
            break;
        case G_QP:
            if ((rc = mlx5_qp_create(&g_bfm, &s_ci.cmd, &s_gsi, MLX5_QP_ST_QP1, 0)) != BFM_OK) return rc;
            s_ci.step = G_INIT;
            break;
        case G_INIT:
            if ((rc = mlx5_qp_rst2init(&g_bfm, &s_ci.cmd, &s_gsi, 1)) != BFM_OK) return rc;
            s_ci.step = G_RTR;
            break;
        case G_RTR:
            if ((rc = mlx5_qp_ud_init2rtr(&g_bfm, &s_ci.cmd, &s_gsi)) != BFM_OK) return rc;
            s_ci.step = G_RTS;
            break;
        case G_RTS:
            if ((rc = mlx5_qp_ud_rtr2rts(&g_bfm, &s_ci.cmd, &s_gsi)) != BFM_OK) return rc;
            for (unsigned i = 0; i < BFM_GSI_PREPOST; i++) gsi_post_recv();
            s_ci.i = 0;
            s_ci.slot_mem = 0;
            s_ci.step = G_SLOT;
            break;
        case G_SLOT: {
            struct bfm_conn *c = &g_bfm_conn[s_ci.i];
            if (!s_ci.slot_mem) {
                if ((rc = slot_alloc(s_ci.i)) != BFM_OK) return rc;
                s_ci.slot_mem = 1;
            }
            /* UAR(ドアベルと BlueFlame の窓)は枠ごとに 1 つ。WC の窓もここで張っておく。 */
            if ((rc = mlx5_alloc_uar(&g_bfm, &s_ci.cmd, &c->qp.uarn)) != BFM_OK) return rc;
            c->qp.has_uar = 1;
            c->db = g_bfm.bar0 + (uint64_t)c->qp.uarn * BFM_PAGE + MLX5_BF_OFFSET;
            c->bf = NULL;
            if (g_bfm_bf_enable && g_bfm.bf_cap) {
                volatile uint8_t *wc = bfm_port_bar0_wc((uint64_t)c->qp.uarn * BFM_PAGE, BFM_PAGE);
                if (wc) c->bf = wc + MLX5_BF_OFFSET;
            }
            s_ci.slot_mem = 0;
            if (++s_ci.i < g_bfm.nconn) break;
            if (g_bfm_bf_enable && g_bfm.bf_cap && g_bfm_conn[0].bf == NULL)
                BFM_LOG("bfm: 注意: UAR を write-combining で張れない。BlueFlame を使わずに動かす\n");
            s_gsi_ready = 1;                          /* 全部そろってから使えるようにする */
            s_ci.step = G_DONE;
            break;
        }
        default:   /* G_DONE */
            return BFM_OK;
        }
    }
}

/*=================================================================
 * 接続を畳み、GSI の送信が出ていくのを待ち(上限 TX_DRAIN_MS)、WC の窓を返す
 * (bfm_shutdown() から。待たずに進める)。使っている接続の枠は、それぞれの状態機械を
 * 畳む手順で進める(UP なら BYE を送り、QP と CQ を消す)。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。
 * ===============================================================*/
int bfm_cm_fini_step(void)
{
    bfm_lock(&g_bfm_ctl_lock);
    if (s_cf.step == F_CONNS) {
        int busy = 0;
        for (unsigned i = 0; i < g_bfm.nconn; i++) {
            struct bfm_conn *c = &g_bfm_conn[i];
            if (c->state == BFM_CS_FREE) continue;
            if (!c->abort) {
                c->abort = 1;
                c->result = BFM_OK;
            }
            if (conn_step(c) == BFM_INPROGRESS) busy = 1;
        }
        if (busy) {
            bfm_unlock(&g_bfm_ctl_lock);
            return BFM_INPROGRESS;
        }
        s_cf.t0 = bfm_port_time_ns();
        s_cf.step = F_DRAIN;
    }
    if (s_cf.step == F_DRAIN) {
        if (s_gsi_ready && s_tx_outstanding > 0 && ms_since(s_cf.t0) < TX_DRAIN_MS && gsi_pump() >= 0 &&
            s_tx_outstanding > 0) {
            bfm_unlock(&g_bfm_ctl_lock);
            return BFM_INPROGRESS;
        }
        for (unsigned i = 0; i < g_bfm.nconn; i++) {
            struct bfm_conn *c = &g_bfm_conn[i];
            if (c->bf) bfm_port_bar0_wc_release((uint64_t)c->qp.uarn * BFM_PAGE, BFM_PAGE);
            c->bf = NULL;
        }
        s_gsi_ready = 0;
        s_cf.step = F_DONE;
    }
    bfm_unlock(&g_bfm_ctl_lock);
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * 公開する関数
 * ------------------------------------------------------------------- */

int bfm_connect(bfm_conn_t **pc, uint32_t peer_ip, const uint8_t peer_mac[6], uint16_t port, uint32_t timeout_ms)
{
    if (pc == NULL) return BFM_E_PARAM;
    if (!s_gsi_ready) return BFM_E_STATE;
    bfm_lock(&g_bfm_ctl_lock);
    struct bfm_conn *c = *pc;
    if (c == NULL) {
        /* 最初の呼び出し: 枠を取り、相手を決める */
        if (peer_ip == 0) peer_ip = g_bfm_peer_ip;
        if (peer_mac == NULL) peer_mac = g_bfm_peer_mac;
        if (peer_ip == 0) {
            bfm_unlock(&g_bfm_ctl_lock);
            BFM_LOG("bfm: 相手の IP が無い(peer_ip も g_bfm_peer_ip も 0)\n");
            return BFM_E_PARAM;
        }
        if (port == 0) port = g_bfm_cm_port;
        c = conn_open(port, ROLE_CONNECT);
        if (c == NULL) {
            bfm_unlock(&g_bfm_ctl_lock);
            return BFM_E_NOMEM;
        }
        c->timeout_ms = timeout_ms ? timeout_ms : REQ_TIMEOUT_MS;
        c->peer_ip = peer_ip;
        memcpy(c->peer_mac, peer_mac, 6);
        if (mac_is_zero(c->peer_mac)) memset(c->peer_mac, 0xff, 6);   /* 知らなければ同報。REP で上書きされる */
        mlx5_gid_from_ipv4(peer_ip, c->peer_gid);
        *pc = c;
    } else if (c->role != ROLE_CONNECT) {
        bfm_unlock(&g_bfm_ctl_lock);
        return BFM_E_PARAM;
    }
    const int rc = conn_step(c);
    if (rc != BFM_INPROGRESS && rc != BFM_OK) *pc = NULL;
    bfm_unlock(&g_bfm_ctl_lock);
    return rc;
}

int bfm_accept(bfm_conn_t **pc, uint16_t port, uint32_t timeout_ms)
{
    if (pc == NULL) return BFM_E_PARAM;
    if (!s_gsi_ready) return BFM_E_STATE;
    bfm_lock(&g_bfm_ctl_lock);
    struct bfm_conn *c = *pc;
    if (c == NULL) {
        /* 最初の呼び出し: 枠を取って REQ を待つ */
        if (port == 0) port = g_bfm_cm_port;
        c = conn_open(port, ROLE_ACCEPT);
        if (c == NULL) {
            bfm_unlock(&g_bfm_ctl_lock);
            return BFM_E_NOMEM;
        }
        c->timeout_ms = timeout_ms;
        *pc = c;
    } else if (c->role != ROLE_ACCEPT) {
        bfm_unlock(&g_bfm_ctl_lock);
        return BFM_E_PARAM;
    }
    const int rc = conn_step(c);
    if (rc != BFM_INPROGRESS && rc != BFM_OK) *pc = NULL;
    bfm_unlock(&g_bfm_ctl_lock);
    return rc;
}

int bfm_disconnect(bfm_conn_t *c)
{
    if (c == NULL) return BFM_E_PARAM;
    bfm_lock(&g_bfm_ctl_lock);
    if (c->state == BFM_CS_FREE) {
        bfm_unlock(&g_bfm_ctl_lock);
        return BFM_E_PARAM;
    }
    if (!c->abort) {
        c->abort = 1;
        c->result = BFM_OK;
    }
    const int rc = conn_step(c);
    bfm_unlock(&g_bfm_ctl_lock);
    return rc;
}

/* GSI に来たものを処理する(相手からの BYE、REP が落ちたときの REQ の送り直し)。
 * bfm_poll() が g_bfm_ctl_lock の中で呼ぶ。 */
int bfm_cm_poll(void)
{
    if (!s_gsi_ready) return 0;
    const int n = gsi_pump();
    return (n > 0) ? n : 0;
}
