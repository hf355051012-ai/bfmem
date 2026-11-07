/* mlx5 のコマンドキュー、FW へのページの受け渡し、EQ(非同期イベント)。
 *
 * コマンドキューは entry 0 だけを使い、1 つずつ同期で出す(冷たい経路なので排他は
 * 持たない。bfm.h の「スレッド安全ではない」の通り、1 つのタスクから呼ぶこと)。 */
#include "bfm_internal.h"

#include <string.h>

/* コマンドの entry(64 バイト)の並び(Linux の struct mlx5_cmd_layout)。 */
#define CMD_TYPE       0u
#define CMD_INLEN      4u
#define CMD_IN_PTR     8u
#define CMD_IN         16u
#define CMD_OUT        32u
#define CMD_OUT_PTR    48u
#define CMD_OUTLEN     56u
#define CMD_TOKEN      60u
#define CMD_SIG        61u
#define CMD_STATUS_OWN 63u
#define CMD_ENTRY_SIZE 64u
#define CMD_XPORT_PCI  7u

/* メールボックスの 1 ブロック(576 バイト。struct mlx5_cmd_prot_block)。 */
#define MBOX_DATA      0u
#define MBOX_NEXT      560u
#define MBOX_BLOCK_NUM 568u
#define MBOX_TOKEN     573u
#define MBOX_SIZE      576u

#define CMD_TIMEOUT_MS 10000u

bfm_dev_t g_bfm;

static volatile uint8_t *mbox_in(const bfm_dev_t *d, uint32_t i)
{
    return d->mbox_in + (uint64_t)i * BFM_MBOX_STRIDE;
}
static volatile uint8_t *mbox_out(const bfm_dev_t *d, uint32_t i)
{
    return d->mbox_out + (uint64_t)i * BFM_MBOX_STRIDE;
}

/*=================================================================
 * コマンドキューを FW に登録する。entry の大きさが 64 バイトであることを確かめ、
 * 物理アドレスを Initialization Segment へ書く。
 * ===============================================================*/
int mlx5_cmd_init(bfm_dev_t *d)
{
    d->cmdq = (volatile uint8_t *)bfm_mem_alloc(BFM_PAGE, BFM_PAGE);
    d->mbox_in = (volatile uint8_t *)bfm_mem_alloc(BFM_MBOX_BLOCKS * BFM_MBOX_STRIDE, BFM_MBOX_STRIDE);
    d->mbox_out = (volatile uint8_t *)bfm_mem_alloc(BFM_MBOX_BLOCKS * BFM_MBOX_STRIDE, BFM_MBOX_STRIDE);
    if (d->cmdq == NULL || d->mbox_in == NULL || d->mbox_out == NULL) return BFM_E_NOMEM;

    const uint32_t l = bfm_mmio_rd32be(d->bar0, MLX5_ISEG_CMDQ_ADDR_L_SZ) & 0xffu;
    const uint32_t log_stride = l & 0xfu;
    if (log_stride != 6u) {
        BFM_LOG("bfm: コマンドの entry の大きさが 64 バイトでない (log_stride=%u)\n", log_stride);
        return BFM_E_HW;
    }
    const uint64_t pa = bfm_dma_addr(d->cmdq);
    bfm_mmio_wr32be(d->bar0, MLX5_ISEG_CMDQ_ADDR_H, (uint32_t)(pa >> 32));
    bfm_mmio_wr32be(d->bar0, MLX5_ISEG_CMDQ_ADDR_L_SZ, (uint32_t)pa);
    bfm_mb();
    bfm_port_delay_us(10000u);
    return BFM_OK;
}

/*=================================================================
 * コマンドを 1 つ出して完了を待つ。入力の 16 バイトを超える分と、出力の 16 バイトを
 * 超える分はメールボックスの鎖で渡す。出力の status が 0 でなければ失敗にする
 * (what をつけて status と syndrome を表示する)。
 * ===============================================================*/
int mlx5_cmd(bfm_dev_t *d, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outlen,
             const char *what)
{
    const uint32_t max = 16u + BFM_MBOX_BLOCKS * 512u;
    if (inlen > max || outlen > max || inlen < 8u || outlen < 8u) {
        BFM_LOG("bfm: %s: コマンドの長さが範囲外 (in=%u out=%u)\n", what, inlen, outlen);
        return BFM_E_PARAM;
    }
    volatile uint8_t *e = d->cmdq;
    for (unsigned i = 0; i < CMD_ENTRY_SIZE; i++) e[i] = 0;
    for (uint32_t i = 0; i < 16u && i < inlen; i++) e[CMD_IN + i] = in[i];

    const uint32_t in_blocks = (inlen > 16u) ? (inlen - 16u + 511u) / 512u : 0u;
    for (uint32_t b = 0; b < in_blocks; b++) {
        volatile uint8_t *m = mbox_in(d, b);
        for (unsigned i = 0; i < MBOX_SIZE; i++) m[i] = 0;
        const uint32_t from = 16u + b * 512u;
        const uint32_t n = (inlen - from > 512u) ? 512u : inlen - from;
        for (uint32_t i = 0; i < n; i++) m[MBOX_DATA + i] = in[from + i];
        bfm_wr32be(m + MBOX_BLOCK_NUM, b);
        if (b + 1u < in_blocks) bfm_wr64be(m + MBOX_NEXT, bfm_dma_addr(mbox_in(d, b + 1u)));
    }
    if (in_blocks) bfm_wr64be(e + CMD_IN_PTR, bfm_dma_addr(mbox_in(d, 0)));
    bfm_wr32be(e + CMD_INLEN, inlen);

    const uint32_t out_blocks = (outlen > 16u) ? (outlen - 16u + 511u) / 512u : 0u;
    for (uint32_t b = 0; b < out_blocks; b++) {
        volatile uint8_t *m = mbox_out(d, b);
        for (unsigned i = 0; i < MBOX_SIZE; i++) m[i] = 0;
        bfm_wr32be(m + MBOX_BLOCK_NUM, b);
        if (b + 1u < out_blocks) bfm_wr64be(m + MBOX_NEXT, bfm_dma_addr(mbox_out(d, b + 1u)));
    }
    if (out_blocks) bfm_wr64be(e + CMD_OUT_PTR, bfm_dma_addr(mbox_out(d, 0)));
    bfm_wr32be(e + CMD_OUTLEN, outlen);
    e[CMD_TYPE] = CMD_XPORT_PCI;
    e[CMD_STATUS_OWN] = 0x1u;        /* 持ち主を FW へ */
    bfm_mb();
    bfm_mmio_wr32be(d->bar0, MLX5_ISEG_CMD_DBELL, 1u);   /* entry 0 */

    const uint16_t opcode = (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
    const uint64_t t0 = bfm_port_time_ns();
    for (;;) {
        if (!(e[CMD_STATUS_OWN] & 0x1u)) break;
        if (bfm_port_time_ns() - t0 > (uint64_t)CMD_TIMEOUT_MS * 1000000u) {
            BFM_LOG("bfm: %s (opcode 0x%x): FW が %u ms 待っても答えない\n", what, opcode, CMD_TIMEOUT_MS);
            return BFM_E_TIMEOUT;
        }
    }
    bfm_rmb();
    const uint8_t delivery = (uint8_t)(e[CMD_STATUS_OWN] >> 1);
    if (delivery != 0) {
        BFM_LOG("bfm: %s (opcode 0x%x): 渡し方の誤り (delivery status 0x%02x)\n", what, opcode, delivery);
        return BFM_E_HW;
    }
    for (uint32_t i = 0; i < 16u && i < outlen; i++) out[i] = e[CMD_OUT + i];
    for (uint32_t b = 0; b < out_blocks; b++) {
        volatile uint8_t *m = mbox_out(d, b);
        const uint32_t from = 16u + b * 512u;
        const uint32_t n = (outlen - from > 512u) ? 512u : outlen - from;
        for (uint32_t i = 0; i < n; i++) out[from + i] = m[MBOX_DATA + i];
    }
    if (out[0] != 0) {
        BFM_LOG("bfm: %s (opcode 0x%x): status 0x%02x syndrome 0x%08x\n", what, opcode, out[0],
                bfm_rd32be(&out[4]));
        return BFM_E_HW;
    }
    return BFM_OK;
}

/* 「16 バイトの入力で byte 9〜11 にオブジェクトの番号」の形の解放のコマンド。 */
int mlx5_destroy_obj(bfm_dev_t *d, uint16_t opcode, uint32_t id, const char *what)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], opcode);
    bfm_wr24be(&in[9], id);
    return mlx5_cmd(d, in, sizeof(in), out, sizeof(out), what);
}

/* ---------------------------------------------------------------------
 * FW へのページの受け渡し
 * ------------------------------------------------------------------- */

#define PAGES_PER_CMD 512u   /* 16 + 512 x 8 = 4112 バイト(メールボックス 8 ブロック)*/

/*=================================================================
 * MANAGE_PAGES(GIVE)で npages ページを渡す。ページは DMA 領域の中の FW 用の
 * 区画(g_bfm_fw_pages_max ページ)から順に出す。返してもらうことはしない。
 * ===============================================================*/
int mlx5_give_pages(bfm_dev_t *d, uint16_t func, int32_t npages)
{
    static uint8_t in[16u + PAGES_PER_CMD * 8u];
    if (npages <= 0) return BFM_OK;
    if ((uint32_t)npages > g_bfm_fw_pages_max - d->fw_pages_used) {
        BFM_LOG("bfm: FW が %d ページ求めたが、残りは %u ページ。g_bfm_fw_pages_max(今 %u)を"
                "少なくとも %u に増やすこと\n", (int)npages, g_bfm_fw_pages_max - d->fw_pages_used,
                g_bfm_fw_pages_max, d->fw_pages_used + (uint32_t)npages);
        return BFM_E_NOMEM;
    }
    uint32_t left = (uint32_t)npages;
    while (left > 0) {
        const uint32_t n = (left > PAGES_PER_CMD) ? PAGES_PER_CMD : left;
        uint8_t out[16];
        memset(in, 0, 16);
        bfm_wr16be(&in[0], MLX5_CMD_MANAGE_PAGES);
        bfm_wr16be(&in[6], 1u);                       /* op_mod = GIVE */
        bfm_wr16be(&in[10], func);
        bfm_wr32be(&in[12], n);
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t pa = bfm_dma_addr(d->fw_pages + (uint64_t)(d->fw_pages_used + i) * BFM_PAGE);
            bfm_wr64be(&in[16u + i * 8u], pa);
        }
        const int rc = mlx5_cmd(d, in, 16u + n * 8u, out, sizeof(out), "MANAGE_PAGES(GIVE)");
        if (rc != BFM_OK) return rc;
        d->fw_pages_used += n;
        left -= n;
    }
    return BFM_OK;
}

/* MANAGE_PAGES(CANT_GIVE)。渡せないと伝えないと FW は要求を出し直し続ける。 */
static void mlx5_cant_give(bfm_dev_t *d, uint16_t func)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_MANAGE_PAGES);
    bfm_wr16be(&in[10], func);
    (void)mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "MANAGE_PAGES(CANT_GIVE)");
}

/* ---------------------------------------------------------------------
 * EQ
 * ------------------------------------------------------------------- */

#define EQE_SIZE        64u
#define EQ_DB_NOARM     0x48u    /* UAR ページの中の EQ のドアベル(arm しない)*/

/*=================================================================
 * EQ を 1 本作る。割り込みは使わない(arm しない)。購読するのは Linux の
 * MLX5_ASYNC_EVENT_MASK と同じ組 + ページ要求。中身は mlx5_eq_poll() で読む。
 * ===============================================================*/
int mlx5_eq_create(bfm_dev_t *d)
{
    const uint32_t nent = 1u << BFM_EQ_LOG;
    const uint32_t bytes = nent * EQE_SIZE;
    const uint32_t npages = (bytes + BFM_PAGE - 1u) / BFM_PAGE;
    static uint8_t in[272 + 8 * 4];
    if (npages > 4u) return BFM_E_PARAM;              /* in[] に載る PAS は 4 ページまで(BFM_EQ_LOG <= 8)*/
    d->eq_buf = (volatile uint8_t *)bfm_mem_alloc(bytes, BFM_PAGE);
    if (d->eq_buf == NULL) return BFM_E_NOMEM;
    for (uint32_t i = 0; i < nent; i++) d->eq_buf[i * EQE_SIZE + EQE_SIZE - 1u] = 1u;  /* owner の初期値 */

    int rc = mlx5_alloc_uar(d, &d->eq_uarn);
    if (rc != BFM_OK) return rc;

    d->eq_mask = (1ull << MLX5_EVT_PATH_MIG) | (1ull << MLX5_EVT_COMM_EST) |
                 (1ull << MLX5_EVT_SQ_DRAINED) | (1ull << MLX5_EVT_CQ_ERROR) |
                 (1ull << MLX5_EVT_WQ_CATAS_ERROR) | (1ull << MLX5_EVT_PATH_MIG_FAILED) |
                 (1ull << MLX5_EVT_WQ_INVAL_REQ) | (1ull << MLX5_EVT_WQ_ACCESS_ERROR) |
                 (1ull << MLX5_EVT_PORT_CHANGE) | (1ull << MLX5_EVT_SRQ_CATAS_ERROR) |
                 (1ull << MLX5_EVT_SRQ_LAST_WQE) | (1ull << MLX5_EVT_SRQ_RQ_LIMIT);
    if (!d->page_request_disable) d->eq_mask |= 1ull << MLX5_EVT_PAGE_REQUEST;

    uint8_t out[16];
    memset(in, 0, sizeof(in));
    bfm_wr16be(&in[0], MLX5_CMD_CREATE_EQ);
    uint8_t *eqc = &in[16];
    eqc[12] = (uint8_t)BFM_EQ_LOG;                    /* log_eq_size */
    bfm_wr24be(&eqc[13], d->eq_uarn);                 /* uar_page */
    eqc[24] = 0;                                      /* log_page_size = 4KiB */
    bfm_wr64be(&in[88], d->eq_mask);                  /* event_bitmask[0] */
    for (uint32_t p = 0; p < npages; p++) bfm_wr64be(&in[272 + p * 8u], bfm_dma_addr(d->eq_buf + p * BFM_PAGE));
    rc = mlx5_cmd(d, in, 272u + npages * 8u, out, sizeof(out), "CREATE_EQ");
    if (rc != BFM_OK) return rc;
    d->eqn = out[11];
    d->eq_cc = 0;
    return BFM_OK;
}

static void mlx5_eq_update_ci(bfm_dev_t *d)
{
    bfm_mmio_wr32be(d->bar0, (uint64_t)d->eq_uarn * BFM_PAGE + EQ_DB_NOARM,
                    (d->eq_cc & 0xffffffu) | (d->eqn << 24));
}

/*=================================================================
 * EQ に溜まったイベントを読む。ページ要求は、1 回の読み取りの最後の要求にだけ
 * 答える(FW は答えが来るまで同じ要求を出し直すので、全部に答えると与えすぎる。
 * vfio_nvme で実機確認済み)。エラーのイベントは接続をエラーにする。
 * 戻り値: 読んだイベントの数。
 * ===============================================================*/
int mlx5_eq_poll(bfm_dev_t *d)
{
    if (d->eq_buf == NULL) return 0;
    const uint32_t nent = 1u << BFM_EQ_LOG;
    int n = 0;
    int32_t page_n = 0;
    uint16_t page_func = 0;
    int page_req = 0;
    while (n < (int)nent) {
        volatile uint8_t *e = d->eq_buf + (d->eq_cc & (nent - 1u)) * EQE_SIZE;
        const uint8_t want = (d->eq_cc & nent) ? 1u : 0u;
        if ((e[EQE_SIZE - 1u] & 1u) != want) break;
        bfm_rmb();
        const uint8_t type = e[1], sub = e[3];
        const volatile uint8_t *data = e + 32;
        switch (type) {
        case MLX5_EVT_PAGE_REQUEST:
            page_func = bfm_rd16be(data + 2);
            page_n = (int32_t)bfm_rd32be(data + 4);
            page_req = 1;
            break;
        case MLX5_EVT_PORT_CHANGE:
            if (sub == 1u || sub == 4u) d->link_up = (sub == 4u);
            BFM_LOG("bfm: リンクが %s になった\n", sub == 4u ? "ACTIVE" : sub == 1u ? "DOWN" : "?");
            break;
        case MLX5_EVT_CQ_ERROR:
            BFM_LOG("bfm: CQ のエラー cqn=%u syndrome=0x%02x\n", bfm_rd24be(data + 1), data[11]);
            break;
        case MLX5_EVT_WQ_CATAS_ERROR:
        case MLX5_EVT_WQ_INVAL_REQ:
        case MLX5_EVT_WQ_ACCESS_ERROR:
        case MLX5_EVT_PATH_MIG_FAILED:
            if (data[20] == 0u) bfm_conn_mark_error(bfm_rd24be(data + 25), "QP のエラーのイベント");
            BFM_LOG("bfm: QP のエラーのイベント type=0x%02x qpn=%u\n", type, bfm_rd24be(data + 25));
            break;
        default:
            break;   /* COMM_EST などは正常な動きでも出る */
        }
        d->eq_cc++;
        n++;
        if ((n & 15) == 0) mlx5_eq_update_ci(d);
    }
    if (n) mlx5_eq_update_ci(d);
    if (page_req) {
        if (page_n > 0) {
            if (mlx5_give_pages(d, page_func, page_n) == BFM_OK)
                BFM_DBG("bfm: FW のページ要求に答えた(%d ページ、計 %u)\n", (int)page_n, d->fw_pages_used);
            else
                mlx5_cant_give(d, page_func);
        } else if (page_n < 0) {
            BFM_DBG("bfm: FW がページの返却を申し出た(%d)。預けたままにする\n", (int)page_n);
        }
    }
    return n;
}
