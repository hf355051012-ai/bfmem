/* 接続の表と、接続ごとの状態機械(確立・張り直し・切断)。bfmem どうしで、ポートごとの
 * GSI = QP1 の上で bfmem だけの短いメッセージを交わす。
 *
 * 接続の表(g_bfm_conn)は ポート x BFM_MAX_PORT_CONN 行で、行ごとに状態機械(step)を持つ。
 * bfm_run()(bfm_core.c)は行の鍵を取れた行について bfm_cm_conn_step() を呼び、状態を 1 つ進める。
 *
 *   K_PORT -> [受け側 K_WAIT_REQ] -> K_CQ -> K_QP -> K_INIT -> [掛け側 K_WAIT_REP] -> K_RTR -> K_RTS
 *          -> [受け側 K_REP] -> K_UP
 *   K_UP でエラー・相手の BYE -> K_DOWN(bfm_reconnect() を待つ)
 *   張り直し・確立の失敗・停止: [UP / ERROR なら K_BYE] -> K_DESTROY -> (張り直しなら確立の最初へ、
 *   失敗なら K_FAILED、停止なら K_PORT)
 * K_PORT では、そのポートの立ち上げ(PCIe・HCA・GSI。bfm_dev_step())を進める。最初に鍵を取ったコアが
 * 最初の手順を処理し、次の手順も、そのとき鍵を取ったコアが処理する。
 *
 * RC QP を繋ぐには、相手の QP 番号と開始 PSN、公開メモリのアドレスと rkey を交換する必要がある。
 * IP のスタックを持たないので、交換は RoCE v2 の UD で、相手の QP1(番号が決まっている唯一の QP)へ送る。
 * 相手は常に bfmem なので、IB CM ではなく次の 3 種類だけを使う。
 *
 *   掛け側: REQ ->(REP が来るまで g_bfm_req_retry_ms ごとに送り直す。相手が立ち上がるまで続ける) <- REP
 *   受け側: <- REQ  QP を RTS にしてから REP ->
 *   切断:   BYE ->(返事は待たない。相手は受けて閉じた扱い(K_DOWN)にする)
 *
 * ポートごとに、IPv4 の小さいほうのノードが掛け側、大きいほうが受け側になる(両方のノードで同じものを
 * 動かせる)。接続 (ポート p, 番号 i) は待ち受けの番号 g_bfm_cm_port + i で対になる。受け側は REP を送る前に
 * RTS にするので、掛け側は REP を受けて RTS にすればすぐ使える(RTU は要らない)。REP が落ちたときは掛け側が
 * REQ を送り直し、受け側は確立済みの接続への REQ に REP を送り直す。どちらの側も、RTR2RTS で相手からの
 * write / read / atomic を許し(メモリの公開)、相手の公開メモリへ転送できる(双方向)。
 *
 * メッセージ(MSG_SIZE バイト、数はビッグエンディアン):
 *   [0..3] 'BFM2'  [4] 種類  [5] 経路 MTU  [6..7] 待ち受けの番号
 *   [8..11] 送り手の接続の id  [12..15] 受け手の接続の id(REQ では 0)
 *   [16..19] QPN  [20..23] 開始 PSN  [24] 受け付ける read / atomic の数
 *   [26..31] 送り手の MAC  [32..35] 送り手の IPv4
 *   [36..39] 公開メモリの rkey  [40..47] 公開メモリの先頭  [48..55] 公開メモリの大きさ
 *   [56..59] 宛先の IPv4
 *
 * ARP を使わない。掛け側が相手の MAC を知らなければ(g_bfm_peer_mac[] が全部 0)、REQ を同報(宛先の MAC が
 * ff:ff:ff:ff:ff:ff)で送り、REP に入った相手の MAC を使う。同報は宛先でない bfmem にも届くので、
 * 受け手は [56..59] が自分の IPv4 でないものを捨てる。
 *
 * 受け取ったメッセージは gsi_pump() がその場で処理する(REP はその接続の枠へ写し、BYE は接続を閉じた
 * 扱いにし、REQ は受け側の接続のために取っておく)。GSI・FW のコマンド・EQ は g_bfm_ctl_lock の中で触る。 */
#include "bfm_internal.h"

#include <string.h>

#define MSG_MAGIC         0x42464d32u /* 'BFM2' */
#define MSG_SIZE          64u
#define MSG_REQ           1u
#define MSG_REP           2u
#define MSG_BYE           3u

#define REQ_STALE_MS      2000u       /* これより古い REQ は掛け側があきらめたとみなして捨てる */
#define TX_DRAIN_MS       100u        /* BYE を出す・出ていくのを待つ上限 */

/* 接続の状態機械の手順(struct bfm_conn の step)。畳む手順へ移る判定に順序を使う(K_BYE より前 = 畳める)。 */
enum {
    K_IDLE = 0,       /* 使わない行 */
    K_PORT,           /* ポートの立ち上げ(PCIe・HCA・公開メモリの MKey・GSI)を進める・待つ */
    K_WAIT_REQ,       /* 受け側: REQ を待つ */
    K_CQ,             /* CREATE_CQ */
    K_QP,             /* CREATE_QP */
    K_INIT,           /* RST2INIT_QP */
    K_WAIT_REP,       /* 掛け側: REQ を送り(送り直し)、REP を待つ */
    K_RTR,            /* INIT2RTR_QP */
    K_RTS,            /* RTR2RTS_QP(相手からのアクセスを許す。atomic_mode が断られたら付けずにもう一度)*/
    K_REP,            /* 受け側: REP を送る */
    K_UP,             /* 使える */
    K_DOWN,           /* エラー・相手が切った(bfm_reconnect() を待つ)*/
    K_BYE,            /* 畳む: 相手へ BYE を送る */
    K_DESTROY,        /* 畳む: QP と CQ を消して枠を空ける */
    K_FAILED          /* 失敗した(bfm_shutdown() まで止まる)*/
};
enum { ROLE_CONNECT = 1, ROLE_ACCEPT = 2 };

struct bfm_conn g_bfm_conn[BFM_MAX_CONN];   /* 接続の表(ポート p の接続 i は p * BFM_MAX_PORT_CONN + i)*/

/* 受け側の接続が待っている REQ(ポートの接続の数まで。同じ掛け側からの送り直しは 1 つに
 * まとめる。一杯なら最も古いものを上書きする。掛け側が送り直すので、落としてもよい)。 */
#define REQ_Q BFM_MAX_PORT_CONN

/* ポートごとの接続管理の状態。 */
struct cm_port {
    bfm_qp_t gsi;                    /* 接続管理(REQ / REP / BYE)を運ぶ GSI の QP(QP1)*/
    volatile uint8_t *gsi_db;        /* GSI の UAR + 0x800(UC)*/
    volatile uint8_t *rx_ring;       /* 受信の枠(2^BFM_GSI_RQ_LOG 個)*/
    volatile uint8_t *tx_buf;        /* 送るメッセージの置き場(BFM_GSI_TXBUFS 個)*/
    uint32_t tx_next;                /* 次に使う送信の置き場の通し番号(BFM_GSI_TXBUFS で割った余りが位置)*/
    uint32_t tx_outstanding;         /* 出して、まだ完了していない送信の数 */
    int gsi_ready;                   /* GSI の QP と置き場がそろい、接続管理を使える */
    struct {
        int      used;               /* 中身がある */
        uint64_t t;                  /* 受けた時刻(ns。古さを見る)*/
        uint8_t  msg[MSG_SIZE];      /* 受けた REQ のメッセージそのもの */
    } req[REQ_Q];                    /* 受けて、まだ受け側の接続に渡していない REQ */
    struct {
        int step;                    /* 立ち上げの状態(G_*)*/
        unsigned i;                  /* 用意している接続の枠(ポートの中の番号)*/
        int slot_mem;                /* 枠 i のメモリを切り出した */
        bfm_cmd_t cmd;               /* 出しているコマンド */
    } ci;                            /* bfm_cm_init_step() の状態 */
    struct {
        int step;                    /* 停止の状態(F_*)*/
        uint64_t t0;                 /* 送信が出ていくのを待ち始めた時刻 */
    } cf;                            /* bfm_cm_fini_step() の状態 */
};
static struct cm_port s_cm[BFM_MAX_PORT];

static int gsi_pump(bfm_dev_t *d);

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

static uint64_t s_slog_t0;                      /* 状態遷移の記録の時刻の起点(接続の表を作った時刻)*/
_Static_assert(sizeof(struct bfm_steplog) == 24, "状態遷移の記録は 1 件 24 バイト(bfm_config.h・README の説明)");

/* 接続 c の状態遷移の記録に from -> to を足す(移したコアも)。直前の 1 件に、そこにとどまった時間を書く。 */
static void steplog_add(struct bfm_conn *c, uint8_t from, uint8_t to)
{
    const uint64_t now = bfm_port_time_ns();
    if (c->slog_cnt != 0u) {
        struct bfm_steplog *p = &c->slog[(c->slog_cnt - 1u) % BFM_STEPLOG_N];
        p->stay = now - p->t;
    }
    struct bfm_steplog *e = &c->slog[c->slog_cnt % BFM_STEPLOG_N];
    e->t = now;
    e->stay = 0;
    e->from = from;
    e->to = to;
    e->cpu = (uint16_t)bfm_port_cpu_id();
    c->slog_cnt++;
}

/* 手順を変える(変わるときは状態遷移の記録に足す)。K_UP にするときは、それまでに書いたデータ経路の
 * 値が bfm_conn() から見えるように release で書く(読む側は acquire)。 */
static void set_step(struct bfm_conn *c, uint8_t step)
{
    if (c->step != step) steplog_add(c, c->step, step);
    __atomic_store_n(&c->step, step, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------
 * メッセージ
 * ------------------------------------------------------------------- */

static uint8_t own_rr(const bfm_dev_t *d)
{
    const uint8_t l = (d->log_max_ra_res_qp > 4u) ? 4u : d->log_max_ra_res_qp;
    return (uint8_t)(1u << l);
}

/* 自分の情報を詰めたメッセージを作る(REQ / REP)。BYE は id までだけ。 */
static void build_msg(uint8_t *m, uint8_t type, const struct bfm_conn *c)
{
    const bfm_dev_t *d = c->dev;
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
    m[24] = own_rr(d);
    memcpy(m + 26, d->mac, 6);
    bfm_wr32be(m + 32, d->ip);
    bfm_wr32be(m + 36, d->rkey);
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
 * GSI の送受信(ポートごと)
 * ------------------------------------------------------------------- */

static void gsi_post_recv(bfm_dev_t *d)
{
    struct cm_port *m = &s_cm[d->port];
    const uint32_t n = 1u << m->gsi.rq_log;
    const uint32_t idx = m->gsi.rq_pc & (n - 1u);
    volatile uint8_t *w = m->gsi.wq + idx * 16u;
    bfm_wr32be(w, BFM_GSI_RX_SLOT);
    bfm_wr32be(w + 4, d->lkey);
    bfm_wr64be(w + 8, bfm_dma_addr(m->rx_ring + idx * BFM_GSI_RX_SLOT));
    m->gsi.rq_pc++;
    bfm_dma_wmb();
    m->gsi.dbr[0] = bfm_cpu_to_be32(m->gsi.rq_pc & 0xffffu);
}

/*=================================================================
 * メッセージ(MSG_SIZE バイト)を、接続 c のポートの GSI から相手の QP1 へ送る。UD の WQE は
 * ctrl(16)+ datagram(48)+ data(16)で 2 WQEBB。送信の置き場が全部使われていれば、
 * GSI の CQ を 1 回読んでも空かなければ、待たずに BFM_INPROGRESS を返す(呼び出し側が後で送り直す)。
 * ===============================================================*/
static int gsi_send(const uint8_t *msg, const struct bfm_conn *c)
{
    bfm_dev_t *d = c->dev;
    struct cm_port *m = &s_cm[d->port];
    if (m->tx_outstanding >= BFM_GSI_TXBUFS) {
        if (gsi_pump(d) < 0) {
            BFM_LOG("bfm: ポート %u: GSI のエラーで送れない\n", d->port);
            return BFM_E_CQE;
        }
        if (m->tx_outstanding >= BFM_GSI_TXBUFS) return BFM_INPROGRESS;
    }
    volatile uint8_t *buf = m->tx_buf + (m->tx_next++ % BFM_GSI_TXBUFS) * MSG_SIZE;
    for (unsigned i = 0; i < MSG_SIZE; i++) buf[i] = msg[i];

    const uint32_t n = 1u << m->gsi.sq_log;
    const uint32_t pc = m->gsi.sq_pc;
    volatile uint8_t *w0 = m->gsi.sq + (pc & (n - 1u)) * MLX5_SEND_WQE_BB;
    volatile uint8_t *w1 = m->gsi.sq + ((pc + 1u) & (n - 1u)) * MLX5_SEND_WQE_BB;
    for (unsigned i = 0; i < MLX5_SEND_WQE_BB; i++) {
        w0[i] = 0;
        w1[i] = 0;
    }
    bfm_wr32be(w0, ((pc & 0xffffu) << 8) | MLX5_OPCODE_SEND);
    bfm_wr32be(w0 + 4, (m->gsi.qpn << 8) | 5u);             /* ds = ctrl 1 + datagram 3 + data 1 */
    w0[11] = MLX5_WQE_CTRL_CQ_UPDATE;
    bfm_wr32be(w0 + 16, MLX5_QP1_QKEY);                      /* av.key.qkey */
    bfm_wr32be(w0 + 24, 0x80000000u | 1u);                   /* extended AV | 宛先 QPN = 1 */
    bfm_wr16be(w0 + 30, mlx5_udp_sport(m->gsi.qpn, 1u));
    for (unsigned i = 0; i < 6u; i++) w0[36 + i] = c->peer_mac[i];
    w0[43] = 64u;                                            /* hop_limit */
    bfm_wr32be(w0 + 44, 1u << 30);                           /* GRH あり、送り元の GID は 0 番 */
    for (unsigned i = 0; i < 16u; i++) w0[48 + i] = c->peer_gid[i];
    bfm_wr32be(w1, MSG_SIZE);
    bfm_wr32be(w1 + 4, d->lkey);
    bfm_wr64be(w1 + 8, bfm_dma_addr(buf));

    m->gsi.sq_pc = pc + 2u;
    m->tx_outstanding++;
    bfm_dma_wmb();
    m->gsi.dbr[1] = bfm_cpu_to_be32(m->gsi.sq_pc & 0xffffu);
    bfm_dma_wmb();
    uint64_t first8;
    memcpy(&first8, (const void *)(uintptr_t)w0, 8);
    bfm_mmio_wr64_raw(m->gsi_db, first8);
    bfm_wc_flush();
    return BFM_OK;
}

/* REQ を取っておく。同じ掛け側(接続の id と IPv4 が同じ)の送り直しなら置き換え、
 * そうでなければ空き(無ければ最も古いもの)に入れる。 */
static void req_put(struct cm_port *m, const uint8_t *msg)
{
    const uint32_t src = bfm_rd32be(msg + 8), ip = bfm_rd32be(msg + 32);
    unsigned k = 0;
    int found = 0;
    for (unsigned i = 0; i < REQ_Q; i++) {
        if (m->req[i].used && bfm_rd32be(m->req[i].msg + 8) == src && bfm_rd32be(m->req[i].msg + 32) == ip) {
            k = i;
            found = 1;
            break;
        }
    }
    for (unsigned i = 0; i < REQ_Q && !found; i++) {
        if (!m->req[i].used) {
            k = i;
            found = 1;
        } else if (m->req[i].t < m->req[k].t) {
            k = i;
        }
    }
    memcpy(m->req[k].msg, msg, MSG_SIZE);
    m->req[k].t = bfm_port_time_ns();
    m->req[k].used = 1;
}

/* ポート d の、待ち受けの番号 port 宛てで相手(g_bfm_peer_ip[])からの REQ のうち、最も古いものを
 * 取り出して msg に写す。古すぎる REQ(掛け側があきらめた)は捨てる。戻り値: 1 = 取り出した、0 = 無い。 */
static int req_take(bfm_dev_t *d, uint16_t port, uint8_t *msg)
{
    struct cm_port *m = &s_cm[d->port];
    int k = -1;
    for (unsigned i = 0; i < REQ_Q; i++) {
        if (!m->req[i].used) continue;
        if (ms_since(m->req[i].t) >= REQ_STALE_MS) {
            m->req[i].used = 0;
            continue;
        }
        if (bfm_rd16be(m->req[i].msg + 6) == port && bfm_rd32be(m->req[i].msg + 32) == g_bfm_peer_ip[d->port] &&
            (k < 0 || m->req[i].t < m->req[k].t))
            k = (int)i;
    }
    if (k < 0) return 0;
    memcpy(msg, m->req[k].msg, MSG_SIZE);
    m->req[k].used = 0;
    return 1;
}

/* ポート d で受け取ったメッセージ 1 つを処理する。 */
static void handle_msg(bfm_dev_t *d, const uint8_t *msg)
{
    const uint8_t type = msg[4];
    const uint32_t src = bfm_rd32be(msg + 8), dst = bfm_rd32be(msg + 12);
    if (bfm_rd32be(msg + 56) != d->ip) return;          /* ほかの bfmem 宛て(同報の REQ)*/
    if (type == MSG_REQ) {
        /* もう受けた掛け側からの送り直し: 確立済みなら REP が落ちたので送り直し、
         * 用意の途中なら捨てる(REP は用意ができたら送る) */
        for (unsigned i = 0; i < d->nconn; i++) {
            struct bfm_conn *c = &d->conn[i];
            if (c->state != BFM_CS_FREE && c->peer_id == src && c->peer_ip == bfm_rd32be(msg + 32)) {
                if (c->state == BFM_CS_UP && c->step == K_UP) {
                    uint8_t rep[MSG_SIZE];
                    build_msg(rep, MSG_REP, c);
                    (void)gsi_send(rep, c);
                }
                return;
            }
        }
        req_put(&s_cm[d->port], msg);
        return;
    }
    for (unsigned i = 0; i < d->nconn; i++) {
        struct bfm_conn *c = &d->conn[i];
        if (c->local_id != dst) continue;
        if (type == MSG_REP && c->step == K_WAIT_REP && !c->got_rep) {
            take_peer(c, msg);
            c->got_rep = 1;
        } else if (type == MSG_BYE && c->peer_id == src &&
                   (c->state == BFM_CS_UP || c->state == BFM_CS_ERROR)) {
            c->state = BFM_CS_CLOSED;
            BFM_LOG("bfm: 相手がポート %u の接続 %u を切った(bfm_reconnect() で張り直すこと)\n", d->port, c->pidx);
        }
        return;
    }
}

/*=================================================================
 * ポート d の GSI の CQ を読み切る。受信は handle_msg() で処理して受信を出し直し、送信の完了は
 * 数を減らすだけ。戻り値: 受け取ったメッセージの数(-1 = GSI のエラー)。
 * ===============================================================*/
static int gsi_pump(bfm_dev_t *d)
{
    struct cm_port *m = &s_cm[d->port];
    const uint32_t n = 1u << m->gsi.cq_log;
    int got = 0;
    for (;;) {
        volatile uint8_t *cqe = m->gsi.cq + (m->gsi.cq_cc & (n - 1u)) * 64u;
        const uint8_t op_own = cqe[63];
        if ((op_own & 1u) != ((m->gsi.cq_cc >> m->gsi.cq_log) & 1u) || (op_own >> 4) == MLX5_CQE_INVALID) break;
        bfm_rmb();
        const uint8_t opcode = (uint8_t)(op_own >> 4);
        const uint32_t byte_cnt = bfm_rd32be(cqe + 44);
        const uint16_t wqe_cnt = bfm_rd16be(cqe + 60);
        const uint8_t synd = cqe[55];
        m->gsi.cq_cc++;
        m->gsi.cq_dbr[0] = bfm_cpu_to_be32(m->gsi.cq_cc & 0xffffffu);
        if (opcode == MLX5_CQE_REQ) {
            if (m->tx_outstanding) m->tx_outstanding--;
        } else if (opcode == MLX5_CQE_RESP_SEND) {
            const uint32_t idx = wqe_cnt & ((1u << m->gsi.rq_log) - 1u);
            const volatile uint8_t *buf = m->rx_ring + idx * BFM_GSI_RX_SLOT + MLX5_GRH_BYTES;
            uint8_t msg[MSG_SIZE];
            for (unsigned i = 0; i < MSG_SIZE; i++) msg[i] = buf[i];
            gsi_post_recv(d);
            if (byte_cnt >= MLX5_GRH_BYTES + MSG_SIZE && bfm_rd32be(msg) == MSG_MAGIC) {
                handle_msg(d, msg);
                got++;
            }
        } else {
            BFM_LOG("bfm: ポート %u: GSI のエラー完了 (opcode 0x%x syndrome 0x%02x)\n", d->port, opcode, synd);
            if (opcode == MLX5_CQE_REQ_ERR && m->tx_outstanding) m->tx_outstanding--;
            if (opcode == MLX5_CQE_RESP_ERR) gsi_post_recv(d);
            return -1;
        }
    }
    return got;
}

/* ---------------------------------------------------------------------
 * 接続の状態機械
 * ------------------------------------------------------------------- */

/* 確立の手順の最初に置く(コマンドはまだ出さない)。役割はポートの IPv4 の大小で決まる。 */
static void conn_begin(struct bfm_conn *c)
{
    const bfm_dev_t *d = c->dev;
    c->state = BFM_CS_SETUP;
    c->role = d->initiator ? ROLE_CONNECT : ROLE_ACCEPT;
    c->abort = 0;
    c->reconnect = 0;
    c->result = BFM_OK;
    c->port = (uint16_t)(g_bfm_cm_port + c->pidx);
    c->local_id = cm_rand32();
    c->peer_id = 0;
    c->got_rep = 0;
    c->peer_ip = g_bfm_peer_ip[d->port];
    memcpy(c->peer_mac, g_bfm_peer_mac[d->port], 6);
    if (mac_is_zero(c->peer_mac)) memset(c->peer_mac, 0xff, 6);   /* 知らなければ同報。REP で上書きされる */
    mlx5_gid_from_ipv4(c->peer_ip, c->peer_gid);
    c->path_mtu = (g_bfm_path_mtu >= 1u && g_bfm_path_mtu <= 5u) ? g_bfm_path_mtu : 3u;
    c->cs_req = g_bfm_scatter_cqe ? 0x11u : 0u;
    c->t0 = bfm_port_time_ns();
    c->t_sent = 0;
    memset(&c->cmd, 0, sizeof(c->cmd));
    set_step(c, (c->role == ROLE_CONNECT) ? K_CQ : K_WAIT_REQ);
}

/* 失敗した: 結果を覚えて畳む手順へ回す(次の bfm_cm_conn_step() の頭で切り替わる)。 */
static void conn_fail(struct bfm_conn *c, int rc)
{
    c->result = rc;
    c->abort = 1;
}

/*=================================================================
 * 接続 c の状態機械を 1 つ進める(1 回の呼び出しで 1 つの手順)。手順は先頭のコメントの図。
 * K_PORT ではそのポートの立ち上げ(bfm_dev_step())を進め、済んだら確立を始める。
 * 待つところ(コマンドの完了・REQ / REP・GSI の送信の空き)では何もせずに戻る。
 * 確立の途中の手順では、最初に EQ を読む(FW のページ要求に答える手順がコマンドの入口を
 * 持っていれば、それを進める)。g_bfm_ctl_lock と c->lock の中で呼ぶ(bfm_shutdown() からは
 * g_bfm_ctl_lock だけ)。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 使える(K_UP)か、停止で畳み終えた。
 *         BFM_E_STATE = エラー・切断のまま(K_DOWN)。そのほかの負 = 失敗した(K_FAILED)。
 * ===============================================================*/
int bfm_cm_conn_step(struct bfm_conn *c)
{
    bfm_dev_t *d = c->dev;
    int rc;
    switch (c->step) {
    case K_IDLE:
        return BFM_E_STATE;
    case K_PORT:
        rc = bfm_dev_step(d);
        if (rc == BFM_INPROGRESS) return rc;
        if (rc != BFM_OK) {
            c->result = rc;
            set_step(c, K_FAILED);
            return rc;
        }
        conn_begin(c);
        BFM_DBG("bfm: ポート %u の接続 %u: 確立を始める(%s)\n", d->port, c->pidx,
                c->role == ROLE_CONNECT ? "掛け側" : "受け側");
        return BFM_INPROGRESS;
    case K_FAILED:
        return c->result;
    default:
        break;
    }
    /* FW のページ要求に答える手順がコマンドの入口を持っていることがあるので、ここでも進める */
    if (c->step != K_UP && c->step != K_DOWN) (void)mlx5_eq_poll(d);
    /* 張り直しを頼まれた(使える・エラーのまま の接続だけ)*/
    if (c->reconnect && !c->abort && (c->step == K_UP || c->step == K_DOWN)) {
        c->abort = 1;
        c->result = BFM_OK;
    }
    /* 畳む手順へ(コマンドを出していれば、その完了を受け取ってから)*/
    if (c->abort && c->step < K_BYE && !c->cmd.posted) {
        set_step(c, (c->state == BFM_CS_UP || c->state == BFM_CS_ERROR) ? K_BYE : K_DESTROY);
        c->t_sent = bfm_port_time_ns();
    }
    switch (c->step) {
    case K_WAIT_REQ: {
        uint8_t msg[MSG_SIZE];
        (void)gsi_pump(d);
        if (!req_take(d, c->port, msg)) return BFM_INPROGRESS;
        take_peer(c, msg);
        if (msg[5] >= 1u && msg[5] < c->path_mtu) c->path_mtu = msg[5];
        set_step(c, K_CQ);
        return BFM_INPROGRESS;
    }
    case K_CQ:
        rc = mlx5_cq_create(d, &c->cmd, c->qp.uarn, c->qp.cq, c->qp.cq_log, c->qp.cq_dbr, &c->qp.cqn);
        if (rc == BFM_INPROGRESS) return rc;
        if (rc != BFM_OK) {
            conn_fail(c, rc);
            return BFM_INPROGRESS;
        }
        c->qp.has_cq = 1;
        c->qp.cq_cc = 0;
        set_step(c, K_QP);
        return BFM_INPROGRESS;
    case K_QP:
        rc = mlx5_qp_create(d, &c->cmd, &c->qp, MLX5_QP_ST_RC, c->cs_req);
        if (rc == BFM_INPROGRESS) return rc;
        if (rc != BFM_OK) {
            conn_fail(c, rc);
            return BFM_INPROGRESS;
        }
        set_step(c, K_INIT);
        return BFM_INPROGRESS;
    case K_INIT:
        rc = mlx5_qp_rst2init(d, &c->cmd, &c->qp, 0);
        if (rc == BFM_INPROGRESS) return rc;
        if (rc != BFM_OK) {
            conn_fail(c, rc);
            return BFM_INPROGRESS;
        }
        set_step(c, (c->role == ROLE_CONNECT) ? K_WAIT_REP : K_RTR);
        return BFM_INPROGRESS;
    case K_WAIT_REP:
        (void)gsi_pump(d);
        if (c->got_rep) {
            set_step(c, K_RTR);
            return BFM_INPROGRESS;
        }
        /* 相手のノードが立ち上がるまで送り直し続ける(時間切れにしない)*/
        if (c->t_sent == 0 || ms_since(c->t_sent) >= g_bfm_req_retry_ms) {
            uint8_t req[MSG_SIZE];
            build_msg(req, MSG_REQ, c);
            rc = gsi_send(req, c);
            if (rc == BFM_OK) {
                if (c->t_sent != 0) BFM_DBG("bfm: ポート %u の接続 %u: REP が来ないので REQ を送り直す\n", d->port, c->pidx);
                c->t_sent = bfm_port_time_ns();
            } else if (rc != BFM_INPROGRESS) {
                conn_fail(c, rc);
            }
        }
        return BFM_INPROGRESS;
    case K_RTR:
        rc = mlx5_qp_init2rtr_rc(d, &c->cmd, &c->qp, c);
        if (rc == BFM_INPROGRESS) return rc;
        if (rc != BFM_OK) {
            conn_fail(c, rc);
            return BFM_INPROGRESS;
        }
        c->amode = d->atomic_mode;
        set_step(c, K_RTS);
        return BFM_INPROGRESS;
    case K_RTS:
        /* RTR2RTS で相手からの write / read / atomic を許す(メモリの公開。受ける準備)*/
        rc = mlx5_qp_rtr2rts_rc(d, &c->cmd, &c->qp, c, c->amode);
        if (rc == BFM_INPROGRESS) return rc;
        if (rc != BFM_OK) {
            if (c->amode != 0u) {
                c->amode = 0u;                  /* atomic_mode を付けずにもう一度 */
            } else {
                conn_fail(c, rc);
            }
            return BFM_INPROGRESS;
        }
        if (c->amode != d->atomic_mode)
            BFM_LOG("bfm: 注意: atomic_mode=%u は断られた。相手からの atomic は受けない\n", d->atomic_mode);
        /* 転送の準備(データ経路の値を求める)*/
        if ((rc = bfm_ops_attach(c)) != BFM_OK) {
            conn_fail(c, rc);
            return BFM_INPROGRESS;
        }
        c->state = BFM_CS_UP;
        BFM_LOG("bfm: ポート %u の接続 %u(%s): qpn %u <-> %u  相手 %u.%u.%u.%u  公開メモリ 0x%llx+0x%llx rkey 0x%08x"
                "  MTU %u  read/atomic %u 本  BlueFlame %s\n",
                d->port, c->pidx, c->role == ROLE_CONNECT ? "掛け側" : "受け側", c->qp.qpn, c->peer_qpn,
                c->peer_ip >> 24, (c->peer_ip >> 16) & 0xffu, (c->peer_ip >> 8) & 0xffu, c->peer_ip & 0xffu,
                (unsigned long long)c->raddr, (unsigned long long)c->rlen, c->rkey, 128u << c->path_mtu,
                c->max_rd, c->bf ? "あり" : "なし");
        if (c->role == ROLE_ACCEPT) {
            set_step(c, K_REP);
            return BFM_INPROGRESS;
        }
        set_step(c, K_UP);
        return BFM_OK;
    case K_REP: {
        uint8_t rep[MSG_SIZE];
        build_msg(rep, MSG_REP, c);
        rc = gsi_send(rep, c);
        if (rc == BFM_INPROGRESS) return rc;
        if (rc != BFM_OK) {
            conn_fail(c, rc);
            return BFM_INPROGRESS;
        }
        set_step(c, K_UP);
        return BFM_OK;
    }
    case K_UP:
        if (c->state == BFM_CS_UP) return BFM_OK;
        BFM_LOG("bfm: ポート %u の接続 %u は使えなくなった(%s)\n", d->port, c->pidx,
                c->state == BFM_CS_CLOSED ? "相手が切った" : "エラー");
        set_step(c, K_DOWN);
        return BFM_E_STATE;
    case K_DOWN:
        return BFM_E_STATE;
    case K_BYE: {
        /* 返事は待たない。出せなければ TX_DRAIN_MS まで出し直し、それでも出なければあきらめる */
        uint8_t bye[MSG_SIZE];
        build_msg(bye, MSG_BYE, c);
        rc = gsi_send(bye, c);
        if (rc == BFM_INPROGRESS && ms_since(c->t_sent) < TX_DRAIN_MS) return rc;
        set_step(c, K_DESTROY);
        return BFM_INPROGRESS;
    }
    case K_DESTROY:
        bfm_ops_detach(c);
        if (mlx5_qp_destroy(d, &c->cmd, &c->qp) == BFM_INPROGRESS) return BFM_INPROGRESS;
        c->state = BFM_CS_FREE;
        c->role = 0;
        c->abort = 0;
        c->local_id = c->peer_id = 0;
        BFM_DBG("bfm: ポート %u の接続 %u を畳んだ\n", d->port, c->pidx);
        if (c->reconnect) {
            BFM_LOG("bfm: ポート %u の接続 %u を張り直す\n", d->port, c->pidx);
            conn_begin(c);
            return BFM_INPROGRESS;
        }
        if (c->result != BFM_OK) {
            BFM_LOG("bfm: ポート %u の接続 %u は確立できなかった(%s)\n", d->port, c->pidx, bfm_strerror(c->result));
            set_step(c, K_FAILED);
            return c->result;
        }
        set_step(c, K_PORT);                     /* 停止(bfm_shutdown())。次の bfm_run() で最初から */
        return BFM_OK;
    default:
        return BFM_E_STATE;
    }
}

/* 公開する状態(BFM_ST_*)。 */
int bfm_cm_conn_state(const struct bfm_conn *c)
{
    switch (__atomic_load_n(&c->step, __ATOMIC_ACQUIRE)) {
    case K_IDLE:   return BFM_ST_UNUSED;
    case K_PORT:   return BFM_ST_INIT;
    case K_UP:     return (c->state == BFM_CS_UP) ? BFM_ST_UP : BFM_ST_DOWN;
    case K_DOWN:   return BFM_ST_DOWN;
    case K_FAILED: return BFM_ST_FAILED;
    default:       return BFM_ST_CONNECT;
    }
}

/*=================================================================
 * 接続の表を作る(bfm_run() の最初。ほかのコアが表を触っていないとき)。ポートごとに
 * g_bfm_port_conn[] 本を使う行にし、K_PORT(ポートの立ち上げ待ち)に置く。前の状態遷移の記録は消え、
 * 時刻の起点をここにする(ポート層を開いた後に呼ぶ。bfm_port_time_ns() を使うため)。
 * 装置(g_bfm_dev[])の port・used・nconn は呼び出し側が先に入れておく。
 * ===============================================================*/
void bfm_cm_table_init(void)
{
    memset(g_bfm_conn, 0, sizeof(g_bfm_conn));
    s_slog_t0 = bfm_port_time_ns();
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        bfm_dev_t *d = &g_bfm_dev[p];
        d->conn = &g_bfm_conn[p * BFM_MAX_PORT_CONN];
        for (unsigned i = 0; i < BFM_MAX_PORT_CONN; i++) {
            struct bfm_conn *c = &d->conn[i];
            c->index = (int)(p * BFM_MAX_PORT_CONN + i);
            c->pidx = (uint16_t)i;
            c->dev = d;
            c->used = (d->used && i < d->nconn) ? 1u : 0u;
            c->step = K_IDLE;
            if (c->used) set_step(c, K_PORT);    /* 最初の遷移 IDLE -> PORT も記録する */
        }
    }
}

/* ---------------------------------------------------------------------
 * ポートの立ち上げと停止(GSI と接続の枠)
 * ------------------------------------------------------------------- */

/* 立ち上げの状態(bfm_cm_init_step())。 */
enum { G_START = 0, G_UAR, G_CQ, G_QP, G_INIT, G_RTR, G_RTS, G_SLOT, G_DONE };

/* 停止の状態(bfm_cm_fini_step())。 */
enum { F_CONNS = 0, F_DRAIN, F_DONE };

/* ポート d の立ち上げ・停止の状態を最初に戻す(ポートの立ち上げを始めるときに呼ぶ)。 */
void bfm_cm_reset(bfm_dev_t *d)
{
    memset(&s_cm[d->port], 0, sizeof(s_cm[d->port]));
}

/* ポート d の接続 i の枠のメモリ(WQ・CQ・ドアベルレコード・中継バッファ)を DMA 領域から切り出す。
 * 行の鍵(c->lock)は、ほかのコアが持っていることがあるので触らない。 */
static int slot_alloc(bfm_dev_t *d, unsigned i)
{
    struct bfm_conn *c = &d->conn[i];
    memset(&c->qp, 0, sizeof(c->qp));
    c->state = BFM_CS_FREE;
    c->bf = NULL;
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
    c->aqd = (g_bfm_qdepth >= 1u && g_bfm_qdepth <= BFM_QD_MAX) ? g_bfm_qdepth : BFM_QD_MAX;
    c->abuf_slot = (g_bfm_async_buf_size < 64u) ? 64u : (g_bfm_async_buf_size + 63u) & ~63u;
    c->abuf = (volatile uint8_t *)bfm_mem_alloc((uint64_t)c->abuf_slot * c->aqd, BFM_PAGE);
    if (!c->qp.wq || !c->qp.dbr || !c->qp.cq || !c->qp.cq_dbr || !c->stage || !c->dif_buf || !c->abuf)
        return BFM_E_NOMEM;
    return BFM_OK;
}

/*=================================================================
 * ポート d の GSI と接続の枠を、待つところまで用意する(ポートの立ち上げから)。GSI の QP を RTS まで
 * 進めて受信を投稿し、接続の枠ごとにメモリと UAR を切り出す(UAR の WC の窓もここで張る)。
 * コマンドの完了を待つところで BFM_INPROGRESS を返す。失敗したときの WC の窓は
 * bfm_cm_fini_step() が返す。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。負 = 失敗。
 * ===============================================================*/
int bfm_cm_init_step(bfm_dev_t *d)
{
    struct cm_port *m = &s_cm[d->port];
    int rc;
    for (;;) {
        switch (m->ci.step) {
        case G_START:
            m->gsi.rq_log = BFM_GSI_RQ_LOG;
            m->gsi.sq_log = BFM_GSI_SQ_LOG;
            m->gsi.cq_log = BFM_GSI_CQ_LOG;
            m->gsi.wq = (volatile uint8_t *)bfm_mem_alloc((16u << BFM_GSI_RQ_LOG) + (MLX5_SEND_WQE_BB << BFM_GSI_SQ_LOG), BFM_PAGE);
            m->gsi.dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
            m->gsi.cq = (volatile uint8_t *)bfm_mem_alloc(64u << BFM_GSI_CQ_LOG, BFM_PAGE);
            m->gsi.cq_dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
            m->rx_ring = (volatile uint8_t *)bfm_mem_alloc((uint64_t)BFM_GSI_RX_SLOT << BFM_GSI_RQ_LOG, BFM_PAGE);
            m->tx_buf = (volatile uint8_t *)bfm_mem_alloc(BFM_GSI_TXBUFS * MSG_SIZE, BFM_PAGE);
            if (!m->gsi.wq || !m->gsi.dbr || !m->gsi.cq || !m->gsi.cq_dbr || !m->rx_ring || !m->tx_buf) return BFM_E_NOMEM;
            m->ci.step = G_UAR;
            break;
        case G_UAR:
            if ((rc = mlx5_alloc_uar(d, &m->ci.cmd, &m->gsi.uarn)) != BFM_OK) return rc;
            m->gsi.has_uar = 1;
            m->gsi_db = d->bar0 + (uint64_t)m->gsi.uarn * BFM_PAGE + MLX5_BF_OFFSET;
            m->ci.step = G_CQ;
            break;
        case G_CQ:
            if ((rc = mlx5_cq_create(d, &m->ci.cmd, m->gsi.uarn, m->gsi.cq, m->gsi.cq_log, m->gsi.cq_dbr,
                                     &m->gsi.cqn)) != BFM_OK) return rc;
            m->gsi.has_cq = 1;
            m->gsi.cq_cc = 0;
            m->ci.step = G_QP;
            break;
        case G_QP:
            if ((rc = mlx5_qp_create(d, &m->ci.cmd, &m->gsi, MLX5_QP_ST_QP1, 0)) != BFM_OK) return rc;
            m->ci.step = G_INIT;
            break;
        case G_INIT:
            if ((rc = mlx5_qp_rst2init(d, &m->ci.cmd, &m->gsi, 1)) != BFM_OK) return rc;
            m->ci.step = G_RTR;
            break;
        case G_RTR:
            if ((rc = mlx5_qp_ud_init2rtr(d, &m->ci.cmd, &m->gsi)) != BFM_OK) return rc;
            m->ci.step = G_RTS;
            break;
        case G_RTS:
            if ((rc = mlx5_qp_ud_rtr2rts(d, &m->ci.cmd, &m->gsi)) != BFM_OK) return rc;
            for (unsigned i = 0; i < BFM_GSI_PREPOST; i++) gsi_post_recv(d);
            m->ci.i = 0;
            m->ci.slot_mem = 0;
            m->ci.step = (d->nconn > 0) ? G_SLOT : G_DONE;
            break;
        case G_SLOT: {
            struct bfm_conn *c = &d->conn[m->ci.i];
            if (!m->ci.slot_mem) {
                if ((rc = slot_alloc(d, m->ci.i)) != BFM_OK) return rc;
                m->ci.slot_mem = 1;
            }
            /* UAR(ドアベルと BlueFlame の窓)は枠ごとに 1 つ。WC の窓もここで張っておく。 */
            if ((rc = mlx5_alloc_uar(d, &m->ci.cmd, &c->qp.uarn)) != BFM_OK) return rc;
            c->qp.has_uar = 1;
            c->db = d->bar0 + (uint64_t)c->qp.uarn * BFM_PAGE + MLX5_BF_OFFSET;
            c->bf = NULL;
            if (g_bfm_bf_enable && d->bf_cap) {
                volatile uint8_t *wc = bfm_port_bar0_wc(d->port, (uint64_t)c->qp.uarn * BFM_PAGE, BFM_PAGE);
                if (wc) c->bf = wc + MLX5_BF_OFFSET;
            }
            m->ci.slot_mem = 0;
            if (++m->ci.i < d->nconn) break;
            if (g_bfm_bf_enable && d->bf_cap && d->conn[0].bf == NULL)
                BFM_LOG("bfm: ポート %u: 注意: UAR を write-combining で張れない。BlueFlame を使わずに動かす\n", d->port);
            m->gsi_ready = 1;                         /* 全部そろってから使えるようにする */
            m->ci.step = G_DONE;
            break;
        }
        default:   /* G_DONE */
            m->gsi_ready = 1;
            return BFM_OK;
        }
    }
}

/*=================================================================
 * ポート d の接続を畳み、GSI の送信が出ていくのを待ち(上限 TX_DRAIN_MS)、WC の窓を返す
 * (ポートを止めるとき。待たずに進める)。使っている接続は、それぞれの状態機械を畳む手順で
 * 進める(UP なら BYE を送り、QP と CQ を消す)。g_bfm_ctl_lock の中で呼ぶ。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。
 * ===============================================================*/
int bfm_cm_fini_step(bfm_dev_t *d)
{
    struct cm_port *m = &s_cm[d->port];
    if (m->cf.step == F_CONNS) {
        int busy = 0;
        for (unsigned i = 0; i < d->nconn; i++) {
            struct bfm_conn *c = &d->conn[i];
            if (c->state == BFM_CS_FREE) continue;
            if (!c->abort) {
                c->abort = 1;
                c->reconnect = 0;
                c->result = BFM_OK;
            }
            if (bfm_cm_conn_step(c) == BFM_INPROGRESS || c->state != BFM_CS_FREE) busy = 1;
        }
        if (busy) return BFM_INPROGRESS;
        m->cf.t0 = bfm_port_time_ns();
        m->cf.step = F_DRAIN;
    }
    if (m->cf.step == F_DRAIN) {
        if (m->gsi_ready && m->tx_outstanding > 0 && ms_since(m->cf.t0) < TX_DRAIN_MS && gsi_pump(d) >= 0 &&
            m->tx_outstanding > 0)
            return BFM_INPROGRESS;
        for (unsigned i = 0; i < d->nconn; i++) {
            struct bfm_conn *c = &d->conn[i];
            if (c->bf) bfm_port_bar0_wc_release(d->port, (uint64_t)c->qp.uarn * BFM_PAGE, BFM_PAGE);
            c->bf = NULL;
        }
        m->gsi_ready = 0;
        m->cf.step = F_DONE;
    }
    return BFM_OK;
}

/* ポート d の GSI に来たものを処理する(相手からの BYE、REP が落ちたときの REQ の送り直し)。
 * bfm_run() が g_bfm_ctl_lock の中で呼ぶ。 */
int bfm_cm_poll(bfm_dev_t *d)
{
    if (!s_cm[d->port].gsi_ready) return 0;
    const int n = gsi_pump(d);
    return (n > 0) ? n : 0;
}

/* ---------------------------------------------------------------------
 * 公開する関数
 * ------------------------------------------------------------------- */

bfm_conn_t *bfm_conn(unsigned port, unsigned i)
{
    if (port >= BFM_MAX_PORT || i >= BFM_MAX_PORT_CONN) return NULL;
    struct bfm_conn *c = &g_bfm_conn[port * BFM_MAX_PORT_CONN + i];
    if (!c->used || __atomic_load_n(&c->step, __ATOMIC_ACQUIRE) != K_UP || c->state != BFM_CS_UP) return NULL;
    return c;
}

int bfm_conn_state(unsigned port, unsigned i)
{
    if (port >= BFM_MAX_PORT || i >= BFM_MAX_PORT_CONN) return BFM_ST_UNUSED;
    const struct bfm_conn *c = &g_bfm_conn[port * BFM_MAX_PORT_CONN + i];
    return c->used ? bfm_cm_conn_state(c) : BFM_ST_UNUSED;
}

int bfm_reconnect(unsigned port, unsigned i)
{
    if (port >= BFM_MAX_PORT || i >= BFM_MAX_PORT_CONN) return BFM_E_PARAM;
    struct bfm_conn *c = &g_bfm_conn[port * BFM_MAX_PORT_CONN + i];
    if (!c->used) return BFM_E_PARAM;
    bfm_lock(&c->lock);                          /* 持たれるのは状態 1 つを進める間だけ */
    const int ok = (c->step == K_UP || c->step == K_DOWN);
    if (ok) c->reconnect = 1;
    bfm_unlock(&c->lock);
    return ok ? BFM_OK : BFM_E_STATE;
}

/* 手順(K_*)の名前(状態遷移の記録の表示に使う)。 */
static const char *step_name(uint8_t s)
{
    static const char *const name[] = {
        [K_IDLE] = "IDLE", [K_PORT] = "PORT", [K_WAIT_REQ] = "WAIT_REQ", [K_CQ] = "CQ", [K_QP] = "QP",
        [K_INIT] = "INIT", [K_WAIT_REP] = "WAIT_REP", [K_RTR] = "RTR", [K_RTS] = "RTS", [K_REP] = "REP",
        [K_UP] = "UP", [K_DOWN] = "DOWN", [K_BYE] = "BYE", [K_DESTROY] = "DESTROY", [K_FAILED] = "FAILED"
    };
    return (s < sizeof(name) / sizeof(name[0]) && name[s] != NULL) ? name[s] : "?";
}

int bfm_conn_steplog(unsigned port, unsigned i)
{
    if (port >= BFM_MAX_PORT || i >= BFM_MAX_PORT_CONN) return BFM_E_PARAM;
    struct bfm_conn *c = &g_bfm_conn[port * BFM_MAX_PORT_CONN + i];
    if (!c->used) return BFM_E_PARAM;

    /* 書き手(状態を進める側)は c->lock か g_bfm_ctl_lock(bfm_shutdown())を持つので、両方を取って写す。
     * 表示は遅いので、鍵を離してから行う。 */
    struct bfm_steplog log[BFM_STEPLOG_N];
    bfm_lock(&c->lock);
    bfm_lock(&g_bfm_ctl_lock);
    const uint32_t cnt = c->slog_cnt;
    const uint8_t now_step = c->step;
    const uint64_t now = bfm_port_time_ns();
    memcpy(log, c->slog, sizeof(log));
    bfm_unlock(&g_bfm_ctl_lock);
    bfm_unlock(&c->lock);

    const uint32_t n = (cnt < BFM_STEPLOG_N) ? cnt : BFM_STEPLOG_N;
    /* ほかのコアの表示と混ざらないよう、1 行は 1 回の BFM_LOG で出す */
    if (cnt > n)
        BFM_LOG("bfm: ポート %u の接続 %u の状態遷移(今 %s、全 %u 回。古いものは上書きされ、ここには最後の %u 回。"
                "時刻は接続の表を作った時から、単位は ms)\n", port, i, step_name(now_step), cnt, n);
    else
        BFM_LOG("bfm: ポート %u の接続 %u の状態遷移(今 %s、全 %u 回。時刻は接続の表を作った時から、単位は ms)\n",
                port, i, step_name(now_step), cnt);
    BFM_LOG("bfm:   番号               時刻  コア  遷移                     とどまった時間\n");
    for (uint32_t k = cnt - n; k < cnt; k++) {
        const struct bfm_steplog *e = &log[k % BFM_STEPLOG_N];
        const uint64_t t = e->t - s_slog_t0;
        const int last = (k + 1u == cnt);
        const uint64_t stay = last ? now - e->t : e->stay;
        BFM_LOG("bfm:   %-6u %9llu.%06llu  %4u  %-8s -> %-10s %9llu.%06llu%s\n", k,
                (unsigned long long)(t / 1000000u), (unsigned long long)(t % 1000000u), e->cpu,
                step_name(e->from), step_name(e->to),
                (unsigned long long)(stay / 1000000u), (unsigned long long)(stay % 1000000u),
                last ? "(今もとどまっている)" : "");
    }
    return BFM_OK;
}
