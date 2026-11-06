/* QP(RC と GSI)の作成・状態遷移・破棄。
 *
 * QPC の欄の位置(qpc の先頭からのバイト。struct mlx5_ifc_qpc_bits のビット位置 / 8)は
 * vfio_nvme で ConnectX-4 / 4 Lx の実機で確かめたものをそのまま使う:
 *   [1] st  [2] pm_state<<3  [5..7] pd  [8] mtu<<5 | log_msg_max
 *   [9] log_rq_size<<3  [10] log_sq_size<<3  [11] ts_format<<6 | rlky(0x10)
 *   [13..15] uar_page  [21..23] remote_qpn  [24..67] primary_address_path
 *   [113] log_sra_max<<5 | retry_count  [114] rnr_retry<<5  [121..123] next_send_psn
 *   [125..127] cqn_snd  [145] log_rra_max<<5 | atomic_mode  [146] rre|rwe|rae
 *   [148] min_rnr_nak  [149..151] next_rcv_psn  [157..159] cqn_rcv  [160..167] dbr_addr
 *   [168..171] q_key  [172] rq_type  [198] cs_req
 * address path(ads)の中:
 *   [8] ack_timeout<<3  [9] src_addr_index  [11] hop_limit  [16..31] rgid
 *   [34..35] udp_sport  [37] vhca_port_num  [38..43] rmac */
#include "bfm_internal.h"

#include <string.h>

#define OPT_RRE  0x0002u
#define OPT_RAE  0x0004u
#define OPT_RWE  0x0008u
#define OPT_PKEY 0x0010u
#define OPT_QKEY 0x0020u
#define OPT_RNR_TIMEOUT 0x0040u
#define OPT_PM_STATE    0x0400u

#define QP_IN_SIZE (24u + MLX5_QPC_BYTES + 16u)

static void qp_in_header(uint8_t *in, uint16_t opcode, uint32_t qpn, uint32_t optpar)
{
    memset(in, 0, QP_IN_SIZE);
    bfm_wr16be(&in[0], opcode);
    bfm_wr24be(&in[9], qpn);
    bfm_wr32be(&in[16], optpar);
}

/* RoCE v2 の UDP の送信元ポート(Linux の rdma_calc_flow_label() + rdma_flow_label_to_udp_sport())。 */
uint16_t mlx5_udp_sport(uint32_t lqpn, uint32_t rqpn)
{
    uint64_t v = (uint64_t)lqpn * (uint64_t)rqpn;
    v ^= v >> 20;
    v ^= v >> 40;
    uint32_t fl = (uint32_t)(v & 0xfffffu);
    uint32_t lo = fl & 0x3fffu;
    lo ^= (fl & 0xfc000u) >> 14;
    return (uint16_t)(lo | 0xc000u);
}

/*=================================================================
 * QP を作る。qp の rq_log / sq_log / cq_log / wq / dbr / cq / cq_dbr / uarn は
 * 呼び出し側が埋めておく。CQ もここで作る(送受信で共有)。
 *   st     : MLX5_QP_ST_RC か MLX5_QP_ST_QP1(GSI)
 *   cs_req : 0x11 で 32 バイトまでの read / atomic の応答を CQE に書かせる
 * ===============================================================*/
int mlx5_qp_create(bfm_dev_t *d, bfm_qp_t *qp, uint8_t st, uint8_t cs_req)
{
    int rc = mlx5_cq_create(d, qp->uarn, qp->cq, qp->cq_log, qp->cq_dbr, &qp->cqn);
    if (rc != BFM_OK) return rc;
    qp->has_cq = 1;
    qp->cq_cc = 0;

    const uint32_t rq_bytes = 16u << qp->rq_log;
    const uint32_t wq_bytes = rq_bytes + (MLX5_SEND_WQE_BB << qp->sq_log);
    const uint32_t npages = wq_bytes / BFM_PAGE;
    qp->sq = qp->wq + rq_bytes;
    memset((void *)(uintptr_t)qp->wq, 0, wq_bytes);
    qp->dbr[0] = 0;
    qp->dbr[1] = 0;
    bfm_port_cache_flush(qp->wq, wq_bytes);
    bfm_port_cache_flush(qp->dbr, 8u);

    static uint8_t in[272 + 8 * 32];
    if (npages > 32u) return BFM_E_PARAM;
    memset(in, 0, sizeof(in));
    bfm_wr16be(&in[0], MLX5_CMD_CREATE_QP);
    uint8_t *qpc = &in[24];
    qpc[1] = st;
    bfm_wr24be(&qpc[5], d->pdn);
    qpc[8] = (uint8_t)((3u << 5) | 20u);                     /* mtu = 1024(後で変える)、log_msg_max = 20 */
    qpc[9] = (uint8_t)(qp->rq_log << 3);                     /* log_rq_stride = 0(16 バイト)*/
    qpc[10] = (uint8_t)(qp->sq_log << 3);
    qpc[11] = (uint8_t)(d->qp_ts_format << 6);
    bfm_wr24be(&qpc[13], qp->uarn);
    bfm_wr24be(&qpc[125], qp->cqn);
    bfm_wr24be(&qpc[157], qp->cqn);
    bfm_wr64be(&qpc[160], bfm_dma_addr(qp->dbr));
    qpc[172] = 0u;                                           /* rq_type = 通常の RQ */
    qpc[198] = cs_req;
    for (uint32_t p = 0; p < npages; p++) bfm_wr64be(&in[272 + p * 8u], bfm_dma_addr(qp->wq + p * BFM_PAGE));
    uint8_t out[16];
    rc = mlx5_cmd(d, in, 272u + npages * 8u, out, sizeof(out), "CREATE_QP");
    if (rc != BFM_OK) {
        (void)mlx5_destroy_obj(d, MLX5_CMD_DESTROY_CQ, qp->cqn, "DESTROY_CQ");
        qp->has_cq = 0;
        return rc;
    }
    qp->qpn = bfm_rd24be(&out[9]);
    qp->has_qp = 1;
    qp->sq_pc = qp->rq_pc = 0;
    qp->local_psn = (uint32_t)(bfm_port_time_ns() >> 6) & 0xffffffu;
    return BFM_OK;
}

/* RST -> INIT。pd・rlky・ポート番号だけを入れる(rre などを入れると ConnectX-4 の FW は
 * BAD_PARAM で断る。vfio_nvme で確認)。UD(GSI)は Q_Key も入れる。 */
int mlx5_qp_rst2init(bfm_dev_t *d, bfm_qp_t *qp, int ud)
{
    uint8_t in[QP_IN_SIZE], out[16];
    qp_in_header(in, MLX5_CMD_RST2INIT_QP, qp->qpn, 0);
    uint8_t *qpc = &in[24];
    bfm_wr24be(&qpc[5], d->pdn);
    qpc[11] = 0x10u;                                         /* rlky */
    qpc[24 + 37] = 1u;                                       /* vhca_port_num */
    if (ud) bfm_wr32be(&qpc[168], MLX5_QP1_QKEY);
    return mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "RST2INIT_QP");
}

/* GSI(UD)を INIT -> RTR -> RTS。相手の情報は送る WQE に載せるので、ここでは要らない。 */
int mlx5_qp_ud_to_rts(bfm_dev_t *d, bfm_qp_t *qp)
{
    uint8_t in[QP_IN_SIZE], out[16];
    qp_in_header(in, MLX5_CMD_INIT2RTR_QP, qp->qpn, OPT_PKEY | OPT_QKEY);
    uint8_t *qpc = &in[24];
    bfm_wr24be(&qpc[5], d->pdn);
    qpc[8] = (uint8_t)((3u << 5) | 20u);
    qpc[24 + 37] = 1u;
    bfm_wr32be(&qpc[168], MLX5_QP1_QKEY);
    int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "INIT2RTR_QP(GSI)");
    if (rc != BFM_OK) return rc;
    qp_in_header(in, MLX5_CMD_RTR2RTS_QP, qp->qpn, 0);
    bfm_wr24be(&in[24 + 121], qp->local_psn);
    return mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "RTR2RTS_QP(GSI)");
}

/* 2 の冪に切り下げた log2(v >= 1)。 */
static uint8_t floor_log2(uint32_t v)
{
    uint8_t l = 0;
    while (v > 1u) {
        v >>= 1;
        l++;
    }
    return l;
}

/* responder として同時に受け付ける read / atomic の数の log2(上限 16)。 */
static uint8_t log_rra(const bfm_dev_t *d)
{
    return (d->log_max_ra_res_qp > 4u) ? 4u : d->log_max_ra_res_qp;
}

/*=================================================================
 * RC を INIT -> RTR。相手の QPN・開始 PSN・経路 MTU と RoCE v2 のアドレス
 * (相手の GID / MAC、UDP の送信元ポート)を入れる。
 * log_rra_max(responder として受け付ける read / atomic の数)はここで入れる
 * (RTR2RTS で書いても FW は保持しない)。
 * ===============================================================*/
int mlx5_qp_init2rtr_rc(bfm_dev_t *d, bfm_qp_t *qp, struct bfm_conn *c)
{
    uint8_t in[QP_IN_SIZE], out[16];
    qp_in_header(in, MLX5_CMD_INIT2RTR_QP, qp->qpn, OPT_RRE | OPT_RAE | OPT_RWE | OPT_PKEY);
    uint8_t *qpc = &in[24];
    uint8_t *ads = &qpc[24];
    bfm_wr24be(&qpc[5], d->pdn);
    qpc[8] = (uint8_t)((c->path_mtu << 5) | 20u);
    bfm_wr24be(&qpc[21], c->peer_qpn);
    qpc[145] = (uint8_t)(log_rra(d) << 5);
    qpc[148] = 12u;                                          /* min_rnr_nak */
    bfm_wr24be(&qpc[149], qp->local_psn);                    /* next_rcv_psn = 自分が名乗った PSN */
    ads[9] = 0u;                                             /* src_addr_index = GID の 0 番 */
    ads[11] = 64u;                                           /* hop_limit */
    memcpy(&ads[16], c->peer_gid, 16);
    bfm_wr16be(&ads[34], mlx5_udp_sport(qp->qpn, c->peer_qpn));
    ads[37] = 1u;
    memcpy(&ads[38], c->peer_mac, 6);
    return mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "INIT2RTR_QP");
}

/*=================================================================
 * RC を RTR -> RTS。相手からの write / read / atomic を許す(rre / rwe / rae と
 * atomic_mode)。requester として出せる read / atomic の数は、自分の能力と相手が
 * 受け付ける数(c->peer_rr)の小さいほう。
 * atomic_mode を付けて断られたら、付けずにもう一度試す(相手からの atomic は受けない)。
 * ===============================================================*/
int mlx5_qp_rtr2rts_rc(bfm_dev_t *d, bfm_qp_t *qp, struct bfm_conn *c)
{
    uint8_t sra_cap = (d->log_max_ra_req_qp > 4u) ? 4u : d->log_max_ra_req_qp;
    uint8_t sra = c->peer_rr ? floor_log2(c->peer_rr) : 0u;
    if (sra > sra_cap) sra = sra_cap;
    c->max_rd = (uint8_t)(1u << sra);

    for (int attempt = 0; attempt < 2; attempt++) {
        const uint8_t amode = attempt ? 0u : d->atomic_mode;
        uint8_t in[QP_IN_SIZE], out[16];
        qp_in_header(in, MLX5_CMD_RTR2RTS_QP, qp->qpn,
                     OPT_RRE | OPT_RAE | OPT_RWE | OPT_PM_STATE | OPT_RNR_TIMEOUT);
        uint8_t *qpc = &in[24];
        qpc[2] = (uint8_t)(MLX5_QP_PM_MIGRATED << 3);
        qpc[24 + 8] = (uint8_t)((g_bfm_ack_timeout & 0x1fu) << 3);
        qpc[24 + 37] = 1u;
        qpc[113] = (uint8_t)((sra << 5) | (g_bfm_retry_count & 0x7u));
        qpc[114] = (uint8_t)(7u << 5);                       /* rnr_retry = 無限 */
        bfm_wr24be(&qpc[121], c->peer_psn);                  /* next_send_psn = 相手が名乗った PSN */
        qpc[145] = (uint8_t)((log_rra(d) << 5) | (amode & 0x0fu));
        qpc[146] = (uint8_t)(0xc0u | (amode ? 0x20u : 0u));  /* rre | rwe | rae */
        const int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "RTR2RTS_QP");
        if (rc == BFM_OK) {
            if (attempt) BFM_LOG("bfm: 注意: atomic_mode=%u は断られた。相手からの atomic は受けない\n",
                                 d->atomic_mode);
            return BFM_OK;
        }
        if (amode == 0u) return rc;
    }
    return BFM_E_HW;
}

/* QP と CQ を壊す(UAR は呼び出し側が返す)。 */
void mlx5_qp_destroy(bfm_dev_t *d, bfm_qp_t *qp)
{
    if (qp->has_qp) (void)mlx5_destroy_obj(d, MLX5_CMD_DESTROY_QP, qp->qpn, "DESTROY_QP");
    if (qp->has_cq) (void)mlx5_destroy_obj(d, MLX5_CMD_DESTROY_CQ, qp->cqn, "DESTROY_CQ");
    qp->has_qp = 0;
    qp->has_cq = 0;
}
