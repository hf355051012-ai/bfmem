/* 接続の確立と切断(IB CM を GSI = QP1 の上で話す)。
 *
 * RC QP を繋ぐには、相手の QP 番号と開始 PSN、公開メモリのアドレスと rkey を
 * 交換する必要がある。IP のスタックを持たないので、交換は RoCE の GSI で IB CM の
 * REQ / REP / RTU を使う。Linux の rdma_cm(librdmacm)とも同じ形なので、試験では
 * Linux の相手(tools/bfm_peer.c)と繋がる。
 *
 *   掛け側 bfm_connect(): REQ ->  <- REP  RTU ->
 *   受け側 bfm_accept():  <- REQ  REP ->  <- RTU
 *   切断   bfm_disconnect(): DREQ -> <- DREP(相手からの DREQ には bfm_poll() が DREP を返す)
 *
 * 公開メモリの情報は private data で渡す(REQ は rdma_cm の cma_hdr 36 バイトの後ろ、
 * REP は先頭から)。ARP を使わないので、相手の MAC も private data に入れる。
 *
 *   [0..3] 'BFM1'  [4..5] 版 = 1  [6..7] 旗(bit0 = MAC あり)
 *   [8..15] 公開メモリの先頭  [16..19] rkey  [20..27] 大きさ  [28..33] MAC
 *   (すべてビッグエンディアン)
 *
 * GSI の受信は 1 本の CQ に全部来るので、拾った MAD はいったん待ち行列に入れ、
 * 各処理が自分の欲しいものを取り出す(古いものは 5 秒で捨てる。相手が再送する)。 */
#include "bfm_internal.h"

#include <string.h>

#define CM_CLASS          0x07u
#define CM_CLASS_VERSION  2u
#define CM_METHOD_SEND    0x03u
#define CM_ATTR_REQ       0x0010u
#define CM_ATTR_MRA       0x0011u
#define CM_ATTR_REJ       0x0012u
#define CM_ATTR_REP       0x0013u
#define CM_ATTR_RTU       0x0014u
#define CM_ATTR_DREQ      0x0015u
#define CM_ATTR_DREP      0x0016u
#define MAD_SIZE          256u
#define MAD_HDR           24u
#define CM_PORT_SPACE     0x0106ull   /* SERVICE_ID の上位。Linux の rdma_cm の port space(RDMA_PS_TCP)と同じ値 */

#define PRIV_MAGIC        0x42464d31u /* 'BFM1' */
#define PRIV_MAGIC_RSHT   0x52534854u /* vfio_nvme の tools/rdma_short_peer(相手としてだけ受ける)*/
#define PRIV_LEN          36u

#define REQ_RETRY_MS      2000u
#define REQ_RETRIES       4u
#define RTU_WAIT_MS       3000u
#define DREP_WAIT_MS      1000u
#define PEND_SLOTS        16u
#define PEND_AGE_MS       5000u

struct bfm_conn g_bfm_conn[BFM_MAX_CONN];

static bfm_qp_t s_gsi;
static volatile uint8_t *s_gsi_db;          /* GSI の UAR + 0x800(UC)*/
static volatile uint8_t *s_rx_ring;         /* 受信の枠(2^BFM_GSI_RQ_LOG 個)*/
static volatile uint8_t *s_tx_buf;          /* 送る MAD の置き場(BFM_GSI_TXBUFS 個)*/
static uint32_t s_tx_next;
static uint32_t s_tx_outstanding;
static int s_gsi_ready;
/* CA GUID の真ん中の 16bit(EUI-64 の 0xfffe の位置)。bfm_init() のたびに乱数で変える。
 * Linux の ib_cm は (CA GUID, QPN) で相手を見分け、立ち上げ直した後の FW は QPN を同じ順に
 * 配り直すので、GUID まで同じだと前の実行の接続の残りと重なり、REJ reason 10(stale
 * connection)で断られる(実機で踏んだ)。 */
static uint16_t s_guid_salt;

static struct {
    uint8_t  used;
    uint16_t attr;
    uint64_t t;
    uint8_t  mad[MAD_SIZE];
} s_pend[PEND_SLOTS];

static uint32_t cm_rand32(void)
{
    static uint64_t x;
    x ^= bfm_port_time_ns() + 0x9e3779b97f4a7c15ull;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (uint32_t)(x >> 16);
}

static uint64_t ms_since(uint64_t t0)
{
    return (bfm_port_time_ns() - t0) / 1000000u;
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
    bfm_port_cache_flush(w, 16u);
    s_gsi.rq_pc++;
    bfm_dma_wmb();
    s_gsi.dbr[0] = bfm_cpu_to_be32(s_gsi.rq_pc & 0xffffu);
    bfm_port_cache_flush(s_gsi.dbr, 8u);
}

/*=================================================================
 * GSI の CQ を読み切る。受信は待ち行列へ写して受信を出し直し、送信の完了は
 * 数を減らすだけ。戻り値: 受け取った MAD の数(-1 = GSI のエラー)。
 * ===============================================================*/
static int gsi_pump(void)
{
    const uint32_t n = 1u << s_gsi.cq_log;
    int got = 0;
    for (;;) {
        volatile uint8_t *cqe = s_gsi.cq + (s_gsi.cq_cc & (n - 1u)) * 64u;
        bfm_port_cache_inval(cqe, 64u);
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
            volatile uint8_t *buf = s_rx_ring + idx * BFM_GSI_RX_SLOT;
            bfm_port_cache_inval(buf, BFM_GSI_RX_SLOT);
            const volatile uint8_t *mad = buf + MLX5_GRH_BYTES;
            if (byte_cnt >= MLX5_GRH_BYTES + MAD_HDR && mad[1] == CM_CLASS) {
                unsigned slot = 0;
                uint64_t oldest = ~0ull;
                for (unsigned i = 0; i < PEND_SLOTS; i++) {
                    if (!s_pend[i].used) { slot = i; break; }
                    if (s_pend[i].t < oldest) { oldest = s_pend[i].t; slot = i; }
                }
                s_pend[slot].used = 1;
                s_pend[slot].attr = bfm_rd16be(mad + 16);
                s_pend[slot].t = bfm_port_time_ns();
                for (unsigned i = 0; i < MAD_SIZE; i++) s_pend[slot].mad[i] = mad[i];
                got++;
            }
            gsi_post_recv();
        } else {
            BFM_LOG("bfm: GSI のエラー完了 (opcode 0x%x syndrome 0x%02x)\n", opcode, synd);
            if (opcode == MLX5_CQE_REQ_ERR && s_tx_outstanding) s_tx_outstanding--;
            if (opcode == MLX5_CQE_RESP_ERR) gsi_post_recv();
            return -1;
        }
    }
    return got;
}

/*=================================================================
 * MAD(256 バイト)を相手の GSI(QPN 1)へ送る。UD の WQE は
 * ctrl(16)+ datagram(48)+ data(16)で 2 WQEBB。
 * ===============================================================*/
static int gsi_send(const uint8_t *mad, const uint8_t peer_gid[16], const uint8_t peer_mac[6])
{
    const uint64_t t0 = bfm_port_time_ns();
    while (s_tx_outstanding >= BFM_GSI_TXBUFS) {
        if (gsi_pump() < 0) {
            BFM_LOG("bfm: GSI のエラーで送れない\n");
            return BFM_E_CQE;
        }
        if (ms_since(t0) > 1000u) {
            BFM_LOG("bfm: GSI の送信が詰まった\n");
            return BFM_E_TIMEOUT;
        }
    }
    volatile uint8_t *buf = s_tx_buf + (s_tx_next++ % BFM_GSI_TXBUFS) * MAD_SIZE;
    for (unsigned i = 0; i < MAD_SIZE; i++) buf[i] = mad[i];
    bfm_port_cache_flush(buf, MAD_SIZE);

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
    for (unsigned i = 0; i < 6u; i++) w0[36 + i] = peer_mac[i];
    w0[43] = 64u;                                            /* hop_limit */
    bfm_wr32be(w0 + 44, 1u << 30);                           /* GRH あり、送り元の GID は 0 番 */
    for (unsigned i = 0; i < 16u; i++) w0[48 + i] = peer_gid[i];
    bfm_wr32be(w1, MAD_SIZE);
    bfm_wr32be(w1 + 4, g_bfm.lkey);
    bfm_wr64be(w1 + 8, bfm_dma_addr(buf));
    bfm_port_cache_flush(w0, MLX5_SEND_WQE_BB);
    bfm_port_cache_flush(w1, MLX5_SEND_WQE_BB);

    s_gsi.sq_pc = pc + 2u;
    s_tx_outstanding++;
    bfm_dma_wmb();
    s_gsi.dbr[1] = bfm_cpu_to_be32(s_gsi.sq_pc & 0xffffu);
    bfm_port_cache_flush(s_gsi.dbr, 8u);
    bfm_dma_wmb();
    uint64_t first8;
    memcpy(&first8, (const void *)(uintptr_t)w0, 8);
    bfm_mmio_wr64_raw(s_gsi_db, first8);
    bfm_wc_flush();
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * 待ち行列
 * ------------------------------------------------------------------- */

typedef int (*cm_match_fn)(const uint8_t *mad, const void *arg);

/* 条件に合う MAD を 1 つ取り出して out へ写す。1 = あった。 */
static int pend_take(uint16_t attr, cm_match_fn match, const void *arg, uint8_t *out)
{
    for (unsigned i = 0; i < PEND_SLOTS; i++) {
        if (!s_pend[i].used || s_pend[i].attr != attr) continue;
        if (match && !match(s_pend[i].mad, arg)) continue;
        memcpy(out, s_pend[i].mad, MAD_SIZE);
        s_pend[i].used = 0;
        return 1;
    }
    return 0;
}

static void pend_age(void)
{
    const uint64_t now = bfm_port_time_ns();
    for (unsigned i = 0; i < PEND_SLOTS; i++) {
        if (s_pend[i].used && now - s_pend[i].t > (uint64_t)PEND_AGE_MS * 1000000u) s_pend[i].used = 0;
    }
}

/* 相手の MAD の REMOTE_COMM_ID(payload の 4〜7)が自分の comm id か。 */
static int match_remote_comm(const uint8_t *mad, const void *arg)
{
    return bfm_rd32be(mad + MAD_HDR + 4) == *(const uint32_t *)arg;
}

static int match_req_port(const uint8_t *mad, const void *arg)
{
    const uint16_t port = *(const uint16_t *)arg;
    return bfm_rd64be(mad + MAD_HDR + 8) == ((CM_PORT_SPACE << 16) | port);
}

/* ---------------------------------------------------------------------
 * CM のメッセージ
 * ------------------------------------------------------------------- */

static void mad_header(uint8_t *mad, uint64_t tid, uint16_t attr)
{
    memset(mad, 0, MAD_SIZE);
    mad[0] = 1u;                         /* base_version */
    mad[1] = CM_CLASS;
    mad[2] = CM_CLASS_VERSION;
    mad[3] = CM_METHOD_SEND;
    bfm_wr64be(mad + 8, tid);
    bfm_wr16be(mad + 16, attr);
}

/* CA GUID: 自分の MAC から作る EUI-64(Linux の ib_cm は (GUID, QPN) で接続を見分ける)。 */
static uint64_t local_guid(void)
{
    const uint8_t *m = g_bfm.mac;
    return ((uint64_t)(m[0] ^ 0x02u) << 56) | ((uint64_t)m[1] << 48) | ((uint64_t)m[2] << 40) |
           ((uint64_t)s_guid_salt << 24) | ((uint64_t)m[3] << 16) | ((uint64_t)m[4] << 8) | m[5];
}

static void put_priv(uint8_t *p)
{
    bfm_wr32be(p, PRIV_MAGIC);
    bfm_wr16be(p + 4, 1u);
    bfm_wr16be(p + 6, 1u);
    bfm_wr64be(p + 8, g_bfm_mr_base);
    bfm_wr32be(p + 16, g_bfm.rkey);
    bfm_wr64be(p + 20, g_bfm_mr_size);
    memcpy(p + 28, g_bfm.mac, 6);
}

/* 相手の private data を読む。1 = 公開メモリの情報があった。MAC があれば mac_ok = 1。 */
static int get_priv(struct bfm_conn *c, const uint8_t *p, int *mac_ok)
{
    *mac_ok = 0;
    const uint32_t magic = bfm_rd32be(p);
    if (magic == PRIV_MAGIC) {
        c->raddr = bfm_rd64be(p + 8);
        c->rkey = bfm_rd32be(p + 16);
        c->rlen = bfm_rd64be(p + 20);
        if (bfm_rd16be(p + 6) & 1u) {
            memcpy(c->peer_mac, p + 28, 6);
            *mac_ok = 1;
        }
        return 1;
    }
    if (magic == PRIV_MAGIC_RSHT) {
        c->raddr = bfm_rd64be(p + 8);
        c->rkey = bfm_rd32be(p + 16);
        c->rlen = bfm_rd32be(p + 20);
        return 1;
    }
    return 0;
}

static uint8_t own_rr(void)
{
    const uint8_t l = (g_bfm.log_max_ra_res_qp > 4u) ? 4u : g_bfm.log_max_ra_res_qp;
    return (uint8_t)(1u << l);
}

static void build_req(const struct bfm_conn *c, uint8_t *mad)
{
    mad_header(mad, c->tid, CM_ATTR_REQ);
    uint8_t *p = mad + MAD_HDR;
    bfm_wr32be(p + 0, c->local_comm_id);
    bfm_wr64be(p + 8, (CM_PORT_SPACE << 16) | c->port);       /* SERVICE_ID */
    bfm_wr64be(p + 16, local_guid());
    bfm_wr24be(p + 32, c->qp.qpn);
    p[35] = own_rr();                                     /* RESPONDER_RESOURCES */
    p[39] = own_rr();                                     /* INITIATOR_DEPTH */
    p[43] = (uint8_t)(20u << 3);                          /* REMOTE_CM_RESPONSE_TIMEOUT、RC */
    bfm_wr24be(p + 44, c->qp.local_psn);
    p[47] = (uint8_t)((20u << 3) | 7u);                   /* LOCAL_CM_RESPONSE_TIMEOUT、RETRY_COUNT */
    bfm_wr16be(p + 48, 0xffffu);                          /* PKEY */
    p[50] = (uint8_t)((c->path_mtu << 4) | 7u);           /* PATH_MTU、RNR_RETRY_COUNT */
    p[51] = (uint8_t)(3u << 4);                           /* MAX_CM_RETRIES */
    memcpy(p + 56, g_bfm.gid, 16);                        /* PRIMARY_LOCAL_PORT_GID */
    memcpy(p + 72, c->peer_gid, 16);                      /* PRIMARY_REMOTE_PORT_GID */
    p[93] = 64u;                                          /* hop limit */
    p[95] = (uint8_t)((g_bfm_ack_timeout & 0x1fu) << 3);  /* LOCAL_ACK_TIMEOUT */
    uint8_t *priv = p + 140;                              /* cma_hdr(36)+ 自分の情報 */
    priv[1] = (uint8_t)(4u << 4);                         /* ip_version = 4 */
    bfm_wr16be(priv + 2, (uint16_t)(0xc000u | (cm_rand32() & 0x3fffu)));
    bfm_wr32be(priv + 16, g_bfm.ip);                      /* src_addr(IPv4 は末尾 4 バイト)*/
    bfm_wr32be(priv + 32, c->peer_ip);                    /* dst_addr */
    put_priv(priv + 36);
}

static void build_rep(const struct bfm_conn *c, uint8_t *mad)
{
    mad_header(mad, c->tid, CM_ATTR_REP);
    uint8_t *p = mad + MAD_HDR;
    bfm_wr32be(p + 0, c->local_comm_id);
    bfm_wr32be(p + 4, c->remote_comm_id);
    bfm_wr24be(p + 12, c->qp.qpn);
    bfm_wr24be(p + 20, c->qp.local_psn);
    p[24] = own_rr();                                     /* RESPONDER_RESOURCES */
    p[25] = c->max_rd;                                    /* INITIATOR_DEPTH */
    p[26] = (uint8_t)(14u << 3);                          /* TARGET_ACK_DELAY */
    p[27] = (uint8_t)(7u << 5);                           /* RNR_RETRY_COUNT */
    bfm_wr64be(p + 28, local_guid());
    put_priv(p + 36);
}

static void build_short(uint8_t *mad, uint64_t tid, uint16_t attr, uint32_t local, uint32_t remote)
{
    mad_header(mad, tid, attr);
    bfm_wr32be(mad + MAD_HDR + 0, local);
    bfm_wr32be(mad + MAD_HDR + 4, remote);
}

/* ---------------------------------------------------------------------
 * 接続の枠
 * ------------------------------------------------------------------- */

static struct bfm_conn *conn_alloc(void)
{
    for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
        if (g_bfm_conn[i].state == BFM_CS_FREE) {
            g_bfm_conn[i].state = BFM_CS_SETUP;
            return &g_bfm_conn[i];
        }
    }
    BFM_LOG("bfm: 接続の枠が足りない(BFM_MAX_CONN = %u)\n", (unsigned)BFM_MAX_CONN);
    return NULL;
}

/* RC QP を作って INIT まで進める。 */
static int conn_create_qp(struct bfm_conn *c)
{
    c->cs_req = g_bfm_scatter_cqe ? 0x11u : 0u;
    int rc = mlx5_qp_create(&g_bfm, &c->qp, MLX5_QP_ST_RC, c->cs_req);
    if (rc == BFM_OK) rc = mlx5_qp_rst2init(&g_bfm, &c->qp, 0);
    return rc;
}

static void conn_release(struct bfm_conn *c)
{
    bfm_ops_detach(c);
    mlx5_qp_destroy(&g_bfm, &c->qp);
    c->state = BFM_CS_FREE;
}

static int conn_to_rts(struct bfm_conn *c)
{
    int rc = mlx5_qp_init2rtr_rc(&g_bfm, &c->qp, c);
    if (rc == BFM_OK) rc = mlx5_qp_rtr2rts_rc(&g_bfm, &c->qp, c);
    return rc;
}

static void conn_log_up(const struct bfm_conn *c, const char *how)
{
    BFM_LOG("bfm: 接続した(%s): qpn %u <-> %u  相手 %u.%u.%u.%u  公開メモリ 0x%llx+0x%llx rkey 0x%08x"
            "  MTU %u  read/atomic %u 本  BlueFlame %s\n", how, c->qp.qpn, c->peer_qpn,
            c->peer_ip >> 24, (c->peer_ip >> 16) & 0xffu, (c->peer_ip >> 8) & 0xffu, c->peer_ip & 0xffu,
            (unsigned long long)c->raddr, (unsigned long long)c->rlen, c->rkey, 128u << c->path_mtu,
            c->max_rd, c->bf ? "あり" : "なし");
}

/* ---------------------------------------------------------------------
 * 公開する関数
 * ------------------------------------------------------------------- */

/*=================================================================
 * GSI を用意する(bfm_init() から)。接続の枠ごとの資源もここで切り出す。
 * ===============================================================*/
int bfm_cm_init(void)
{
    int rc;
    memset(&s_gsi, 0, sizeof(s_gsi));
    memset(s_pend, 0, sizeof(s_pend));
    s_tx_next = s_tx_outstanding = 0;
    s_guid_salt = (uint16_t)cm_rand32();
    s_gsi.rq_log = BFM_GSI_RQ_LOG;
    s_gsi.sq_log = BFM_GSI_SQ_LOG;
    s_gsi.cq_log = BFM_GSI_CQ_LOG;
    s_gsi.wq = (volatile uint8_t *)bfm_mem_alloc((16u << BFM_GSI_RQ_LOG) + (MLX5_SEND_WQE_BB << BFM_GSI_SQ_LOG), BFM_PAGE);
    s_gsi.dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
    s_gsi.cq = (volatile uint8_t *)bfm_mem_alloc(64u << BFM_GSI_CQ_LOG, BFM_PAGE);
    s_gsi.cq_dbr = (volatile uint32_t *)bfm_mem_alloc(64u, 64u);
    s_rx_ring = (volatile uint8_t *)bfm_mem_alloc((uint64_t)BFM_GSI_RX_SLOT << BFM_GSI_RQ_LOG, BFM_PAGE);
    s_tx_buf = (volatile uint8_t *)bfm_mem_alloc(BFM_GSI_TXBUFS * MAD_SIZE, BFM_PAGE);
    if (!s_gsi.wq || !s_gsi.dbr || !s_gsi.cq || !s_gsi.cq_dbr || !s_rx_ring || !s_tx_buf) return BFM_E_NOMEM;

    if ((rc = mlx5_alloc_uar(&g_bfm, &s_gsi.uarn)) != BFM_OK) return rc;
    s_gsi.has_uar = 1;
    s_gsi_db = g_bfm.bar0 + (uint64_t)s_gsi.uarn * BFM_PAGE + MLX5_BF_OFFSET;
    if ((rc = mlx5_qp_create(&g_bfm, &s_gsi, MLX5_QP_ST_QP1, 0)) != BFM_OK) return rc;
    if ((rc = mlx5_qp_rst2init(&g_bfm, &s_gsi, 1)) != BFM_OK) return rc;
    if ((rc = mlx5_qp_ud_to_rts(&g_bfm, &s_gsi)) != BFM_OK) return rc;
    for (unsigned i = 0; i < BFM_GSI_PREPOST; i++) gsi_post_recv();

    for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
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
        if (!c->qp.wq || !c->qp.dbr || !c->qp.cq || !c->qp.cq_dbr || !c->stage) {
            rc = BFM_E_NOMEM;
            goto fail;
        }
        /* UAR(ドアベルと BlueFlame の窓)は枠ごとに 1 つ。WC の窓もここで張っておく。 */
        if ((rc = mlx5_alloc_uar(&g_bfm, &c->qp.uarn)) != BFM_OK) goto fail;
        c->qp.has_uar = 1;
        c->db = g_bfm.bar0 + (uint64_t)c->qp.uarn * BFM_PAGE + MLX5_BF_OFFSET;
        c->bf = NULL;
        if (g_bfm_bf_enable && g_bfm.bf_cap) {
            volatile uint8_t *wc = bfm_port_bar0_wc((uint64_t)c->qp.uarn * BFM_PAGE, BFM_PAGE);
            if (wc) c->bf = wc + MLX5_BF_OFFSET;
        }
    }
    if (g_bfm_bf_enable && g_bfm.bf_cap && g_bfm_conn[0].bf == NULL)
        BFM_LOG("bfm: 注意: UAR を write-combining で張れない。BlueFlame を使わずに動かす\n");
    s_gsi_ready = 1;                                  /* 全部そろってから使えるようにする */
    return BFM_OK;
fail:
    /* 張った WC の窓を返す(HCA は呼び出し側の cleanup() が止める) */
    for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
        struct bfm_conn *c = &g_bfm_conn[i];
        if (c->bf) bfm_port_bar0_wc_release((uint64_t)c->qp.uarn * BFM_PAGE, BFM_PAGE);
        c->bf = NULL;
    }
    return rc;
}

void bfm_cm_fini(void)
{
    for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
        struct bfm_conn *c = &g_bfm_conn[i];
        if (c->state != BFM_CS_FREE) (void)bfm_disconnect(c);
        if (c->qp.has_uar) {
            if (c->bf) bfm_port_bar0_wc_release((uint64_t)c->qp.uarn * BFM_PAGE, BFM_PAGE);
            c->bf = NULL;
        }
    }
    s_gsi_ready = 0;
}

bfm_conn_t *bfm_connect(uint32_t peer_ip, const uint8_t peer_mac[6], uint16_t port, uint32_t timeout_ms)
{
    if (!s_gsi_ready) return NULL;
    if (peer_ip == 0) {
        peer_ip = g_bfm_peer_ip;
        peer_mac = g_bfm_peer_mac;
    }
    if (peer_mac == NULL) peer_mac = g_bfm_peer_mac;
    if (peer_ip == 0) {
        BFM_LOG("bfm: 相手の IP が無い(peer_ip も g_bfm_peer_ip も 0)\n");
        return NULL;
    }
    if (port == 0) port = g_bfm_cm_port;
    if (timeout_ms == 0) timeout_ms = (REQ_RETRIES + 1u) * REQ_RETRY_MS;
    struct bfm_conn *c = conn_alloc();
    if (c == NULL) return NULL;
    c->peer_ip = peer_ip;
    memcpy(c->peer_mac, peer_mac, 6);
    mlx5_gid_from_ipv4(peer_ip, c->peer_gid);
    c->port = port;
    c->path_mtu = (g_bfm_path_mtu >= 1u && g_bfm_path_mtu <= 5u) ? g_bfm_path_mtu : 3u;
    if (conn_create_qp(c) != BFM_OK) {
        conn_release(c);
        return NULL;
    }
    c->local_comm_id = cm_rand32();
    c->tid = ((uint64_t)cm_rand32() << 32) | cm_rand32();

    uint8_t mad[MAD_SIZE];
    const uint64_t t_start = bfm_port_time_ns();
    uint64_t t_sent = 0;
    unsigned sent = 0;
    for (;;) {
        if (sent == 0 || ms_since(t_sent) > REQ_RETRY_MS) {
            if (sent > REQ_RETRIES || ms_since(t_start) > timeout_ms) {
                BFM_LOG("bfm: 接続できない(REP が来ない)\n");
                goto fail;
            }
            build_req(c, mad);
            if (gsi_send(mad, c->peer_gid, c->peer_mac) != BFM_OK) goto fail;
            t_sent = bfm_port_time_ns();
            sent++;
            BFM_DBG("bfm: REQ を送った(%u 回目)\n", sent);
        }
        (void)gsi_pump();
        if (pend_take(CM_ATTR_REJ, match_remote_comm, &c->local_comm_id, mad)) {
            BFM_LOG("bfm: 相手が接続を断った(REJ reason %u%s)\n", bfm_rd16be(mad + MAD_HDR + 10),
                    bfm_rd16be(mad + MAD_HDR + 10) == 8u ? ": その番号で待ち受けていない" : "");
            goto fail;
        }
        if (pend_take(CM_ATTR_REP, match_remote_comm, &c->local_comm_id, mad)) break;
    }
    {
        const uint8_t *p = mad + MAD_HDR;
        int mac_ok;
        c->remote_comm_id = bfm_rd32be(p + 0);
        c->peer_qpn = bfm_rd24be(p + 12);
        c->peer_psn = bfm_rd24be(p + 20);
        c->peer_rr = p[24] ? p[24] : 1u;
        if (!get_priv(c, p + 36, &mac_ok)) {
            BFM_LOG("bfm: REP の private data に公開メモリの情報が無い(相手が bfmem ではない)\n");
            goto fail;
        }
    }
    if (conn_to_rts(c) != BFM_OK) goto fail;
    build_short(mad, c->tid, CM_ATTR_RTU, c->local_comm_id, c->remote_comm_id);
    if (gsi_send(mad, c->peer_gid, c->peer_mac) != BFM_OK) goto fail;
    if (bfm_ops_attach(c) != BFM_OK) goto fail;
    c->state = BFM_CS_UP;
    conn_log_up(c, "掛け側");
    return c;
fail:
    conn_release(c);
    return NULL;
}

bfm_conn_t *bfm_accept(uint16_t port, uint32_t timeout_ms)
{
    if (!s_gsi_ready) return NULL;
    if (port == 0) port = g_bfm_cm_port;
    uint8_t mad[MAD_SIZE];
    const uint64_t t0 = bfm_port_time_ns();
    for (;;) {
        (void)gsi_pump();
        (void)bfm_cm_poll();
        if (pend_take(CM_ATTR_REQ, match_req_port, &port, mad)) break;
        if (timeout_ms && ms_since(t0) > timeout_ms) return NULL;
    }
    struct bfm_conn *c = conn_alloc();
    if (c == NULL) return NULL;
    const uint8_t *p = mad + MAD_HDR;
    const uint8_t *priv = p + 140;
    int mac_ok;
    c->port = port;
    c->tid = bfm_rd64be(mad + 8);
    c->remote_comm_id = bfm_rd32be(p + 0);
    c->peer_qpn = bfm_rd24be(p + 32);
    c->peer_rr = p[35] ? p[35] : 1u;
    c->peer_psn = bfm_rd24be(p + 44);
    const uint8_t peer_mtu = (uint8_t)(p[50] >> 4);
    c->path_mtu = (g_bfm_path_mtu >= 1u && g_bfm_path_mtu <= 5u) ? g_bfm_path_mtu : 3u;
    if (peer_mtu >= 1u && peer_mtu < c->path_mtu) c->path_mtu = peer_mtu;
    memcpy(c->peer_gid, p + 56, 16);
    c->peer_ip = bfm_rd32be(priv + 16);
    if (!get_priv(c, priv + 36, &mac_ok)) {
        BFM_LOG("bfm: REQ の private data に公開メモリの情報が無い。相手のメモリには触れない接続になる\n");
        c->raddr = c->rlen = 0;
        c->rkey = 0;
    }
    if (!mac_ok) {
        static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        if (memcmp(g_bfm_peer_mac, zero, 6) == 0) {
            BFM_LOG("bfm: 相手の MAC が分からない(private data に無く、g_bfm_peer_mac も 0)\n");
            c->state = BFM_CS_FREE;
            return NULL;
        }
        memcpy(c->peer_mac, g_bfm_peer_mac, 6);
    }
    if (conn_create_qp(c) != BFM_OK || conn_to_rts(c) != BFM_OK) goto fail;
    c->local_comm_id = cm_rand32();
    build_rep(c, mad);
    if (gsi_send(mad, c->peer_gid, c->peer_mac) != BFM_OK) goto fail;
    if (bfm_ops_attach(c) != BFM_OK) goto fail;
    c->state = BFM_CS_UP;

    /* RTU を待つ。来なくても QP は RTS なので使える(相手の最初のパケットで確立する)。 */
    const uint64_t t1 = bfm_port_time_ns();
    int rtu = 0;
    while (ms_since(t1) < RTU_WAIT_MS) {
        (void)gsi_pump();
        if (pend_take(CM_ATTR_RTU, match_remote_comm, &c->local_comm_id, mad)) {
            rtu = 1;
            break;
        }
    }
    if (!rtu) BFM_LOG("bfm: 注意: RTU が来ない(QP は使える状態なので進む)\n");
    conn_log_up(c, "受け側");
    return c;
fail:
    conn_release(c);
    return NULL;
}

int bfm_disconnect(bfm_conn_t *c)
{
    if (c == NULL || c->state == BFM_CS_FREE) return BFM_E_PARAM;
    if (c->state == BFM_CS_UP || c->state == BFM_CS_ERROR) {
        uint8_t mad[MAD_SIZE];
        const uint64_t tid = ((uint64_t)cm_rand32() << 32) | cm_rand32();
        build_short(mad, tid, CM_ATTR_DREQ, c->local_comm_id, c->remote_comm_id);
        bfm_wr24be(mad + MAD_HDR + 8, c->peer_qpn);     /* REMOTE_QPN */
        if (gsi_send(mad, c->peer_gid, c->peer_mac) == BFM_OK) {
            const uint64_t t0 = bfm_port_time_ns();
            while (ms_since(t0) < DREP_WAIT_MS) {
                (void)gsi_pump();
                if (pend_take(CM_ATTR_DREP, match_remote_comm, &c->local_comm_id, mad)) break;
                /* 相手も同時に DREQ を出したら DREP を返す。 */
                if (pend_take(CM_ATTR_DREQ, match_remote_comm, &c->local_comm_id, mad)) {
                    const uint64_t rtid = bfm_rd64be(mad + 8);
                    build_short(mad, rtid, CM_ATTR_DREP, c->local_comm_id, c->remote_comm_id);
                    (void)gsi_send(mad, c->peer_gid, c->peer_mac);
                    break;
                }
            }
        }
    }
    conn_release(c);
    BFM_DBG("bfm: 切断した\n");
    return BFM_OK;
}

/*=================================================================
 * GSI に来たものを処理する: 相手からの DREQ には DREP を返して接続を閉じた扱いにし、
 * 確立済みの接続への REQ の再送には REP を送り直す(RTU が落ちたとき)。
 * ===============================================================*/
int bfm_cm_poll(void)
{
    if (!s_gsi_ready) return 0;
    int n = 0;
    uint8_t mad[MAD_SIZE];
    (void)gsi_pump();
    while (pend_take(CM_ATTR_DREQ, NULL, NULL, mad)) {
        const uint32_t target = bfm_rd32be(mad + MAD_HDR + 4);
        const uint64_t tid = bfm_rd64be(mad + 8);
        for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
            struct bfm_conn *c = &g_bfm_conn[i];
            /* 作っている途中の枠(SETUP)は local_comm_id がまだ前の値なので見ない */
            if (c->state == BFM_CS_FREE || c->state == BFM_CS_SETUP || c->local_comm_id != target) continue;
            uint8_t rep[MAD_SIZE];
            build_short(rep, tid, CM_ATTR_DREP, c->local_comm_id, c->remote_comm_id);
            (void)gsi_send(rep, c->peer_gid, c->peer_mac);
            if (c->state == BFM_CS_UP || c->state == BFM_CS_ERROR) {
                c->state = BFM_CS_CLOSED;
                BFM_LOG("bfm: 相手が接続 %u を切った(bfm_disconnect() で返すこと)\n", c->index);
            }
            n++;
        }
    }
    for (unsigned i = 0; i < PEND_SLOTS; i++) {
        if (!s_pend[i].used || s_pend[i].attr != CM_ATTR_REQ) continue;
        const uint8_t *p = s_pend[i].mad + MAD_HDR;
        for (unsigned k = 0; k < BFM_MAX_CONN; k++) {
            struct bfm_conn *c = &g_bfm_conn[k];
            if (c->state != BFM_CS_UP || c->remote_comm_id != bfm_rd32be(p) ||
                memcmp(c->peer_gid, p + 56, 16) != 0) continue;
            uint8_t rep[MAD_SIZE];
            build_rep(c, rep);
            (void)gsi_send(rep, c->peer_gid, c->peer_mac);
            s_pend[i].used = 0;
            n++;
            break;
        }
    }
    pend_age();
    return n;
}
