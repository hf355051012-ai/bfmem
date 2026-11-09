/* mlx5 のコマンドキュー、FW へのページの受け渡し、EQ(非同期イベント)。
 *
 * コマンドキューは entry 0 だけを使う。コマンドは待たずに扱う: mlx5_cmd_run() は、まだ出していなければ
 * 出し、出してあれば完了を 1 回だけ見て、終わっていなければ BFM_INPROGRESS を返す。呼び出し側は
 * 同じ bfm_cmd_t と同じ引数で呼び直して完了を受け取る。entry は 1 つなので、出している間は
 * その bfm_cmd_t が持ち主になり(d->cmd_owner)、ほかの bfm_cmd_t からの呼び出しは出さずに
 * BFM_INPROGRESS を返す(持ち主が完了を受け取るまで待たされる)。
 * ここでは排他を持たないので、複数のコアから使うときは呼び出し側が g_bfm_ctl_lock を取る。 */
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

bfm_dev_t g_bfm;                            /* 装置の状態(使う ConnectX は 1 枚だけ)*/

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
 * 物理アドレスを Initialization Segment へ書く。FW が受け付けるまでの 10ms は
 * 呼び出し側が待つ(mlx5_hca_init_step())。
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
    return BFM_OK;
}

/* entry とメールボックスに入力を写し、ドアベルを鳴らす。 */
static void cmd_post(bfm_dev_t *d, const uint8_t *in, uint32_t inlen, uint32_t outlen)
{
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
}

/* 完了した entry とメールボックスから出力を読み、status を見る。 */
static int cmd_finish(bfm_dev_t *d, const bfm_cmd_t *j, uint8_t *out)
{
    volatile uint8_t *e = d->cmdq;
    const uint32_t outlen = j->outlen;
    bfm_rmb();
    const uint8_t delivery = (uint8_t)(e[CMD_STATUS_OWN] >> 1);
    if (delivery != 0) {
        BFM_LOG("bfm: %s (opcode 0x%x): 渡し方の誤り (delivery status 0x%02x)\n", j->what, j->opcode, delivery);
        return BFM_E_HW;
    }
    const uint32_t out_blocks = (outlen > 16u) ? (outlen - 16u + 511u) / 512u : 0u;
    for (uint32_t i = 0; i < 16u && i < outlen; i++) out[i] = e[CMD_OUT + i];
    for (uint32_t b = 0; b < out_blocks; b++) {
        volatile uint8_t *m = mbox_out(d, b);
        const uint32_t from = 16u + b * 512u;
        const uint32_t n = (outlen - from > 512u) ? 512u : outlen - from;
        for (uint32_t i = 0; i < n; i++) out[from + i] = m[MBOX_DATA + i];
    }
    if (out[0] != 0) {
        BFM_LOG("bfm: %s (opcode 0x%x): status 0x%02x syndrome 0x%08x\n", j->what, j->opcode, out[0],
                bfm_rd32be(&out[4]));
        return BFM_E_HW;
    }
    return BFM_OK;
}

/*=================================================================
 * コマンドを 1 つ、待たずに進める。j がまだ出していなければ、entry が空いていれば出す
 * (入力の 16 バイトを超える分と、出力の 16 バイトを超える分はメールボックスの鎖で渡す)。
 * 出してあれば完了を 1 回だけ見る。完了していれば出力を out に写し、status が 0 でなければ
 * 失敗にする(what をつけて status と syndrome を表示する)。
 * 戻り値: BFM_INPROGRESS = まだ(同じ j と引数で呼び直す)。BFM_OK = 成功。負 = 失敗。
 * 出した後の呼び出しでは in を見ない(出すときに写してある)。
 * ===============================================================*/
int mlx5_cmd_run(bfm_dev_t *d, bfm_cmd_t *j, const uint8_t *in, uint32_t inlen, uint8_t *out,
                 uint32_t outlen, const char *what)
{
    if (!j->posted) {
        const uint32_t max = 16u + BFM_MBOX_BLOCKS * 512u;
        if (inlen > max || outlen > max || inlen < 8u || outlen < 8u) {
            BFM_LOG("bfm: %s: コマンドの長さが範囲外 (in=%u out=%u)\n", what, inlen, outlen);
            return BFM_E_PARAM;
        }
        if (d->cmd_dead) {
            BFM_LOG("bfm: %s: FW が前のコマンドに答えなかったので出さない\n", what);
            return BFM_E_HW;
        }
        if (d->cmd_owner != NULL) return BFM_INPROGRESS;   /* ほかの手順が entry を使っている */
        cmd_post(d, in, inlen, outlen);
        j->posted = 1;
        j->opcode = (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
        j->outlen = outlen;
        j->what = what;
        j->t0 = bfm_port_time_ns();
        d->cmd_owner = j;
    }
    if (d->cmdq[CMD_STATUS_OWN] & 0x1u) {
        if (bfm_port_time_ns() - j->t0 <= (uint64_t)CMD_TIMEOUT_MS * 1000000u) return BFM_INPROGRESS;
        BFM_LOG("bfm: %s (opcode 0x%x): FW が %u ms 待っても答えない\n", j->what, j->opcode, CMD_TIMEOUT_MS);
        d->cmd_dead = 1;                          /* entry は FW が持ったまま。もう使わない */
        j->posted = 0;
        d->cmd_owner = NULL;
        return BFM_E_TIMEOUT;
    }
    j->posted = 0;
    d->cmd_owner = NULL;
    return cmd_finish(d, j, out);
}

/*=================================================================
 * 出したまま持ち主が受け取っていないコマンドを、完了を待って捨てる(止めるときに使う)。
 * 戻り値: BFM_INPROGRESS = FW がまだ持っている。BFM_OK = entry は空いた。
 * ===============================================================*/
int mlx5_cmd_drain(bfm_dev_t *d)
{
    bfm_cmd_t *j = d->cmd_owner;
    if (j == NULL) return BFM_OK;
    if (d->cmdq[CMD_STATUS_OWN] & 0x1u) {
        if (bfm_port_time_ns() - j->t0 <= (uint64_t)CMD_TIMEOUT_MS * 1000000u) return BFM_INPROGRESS;
        BFM_LOG("bfm: %s (opcode 0x%x): FW が %u ms 待っても答えない\n", j->what, j->opcode, CMD_TIMEOUT_MS);
        d->cmd_dead = 1;
    }
    j->posted = 0;
    d->cmd_owner = NULL;
    return BFM_OK;
}

/* 「16 バイトの入力で byte 9〜11 にオブジェクトの番号」の形の解放のコマンド。 */
int mlx5_destroy_obj(bfm_dev_t *d, bfm_cmd_t *j, uint16_t opcode, uint32_t id, const char *what)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], opcode);
    bfm_wr24be(&in[9], id);
    return mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), what);
}

/* ---------------------------------------------------------------------
 * FW へのページの受け渡し
 * ------------------------------------------------------------------- */

#define PAGES_PER_CMD 512u   /* 16 + 512 x 8 = 4112 バイト(メールボックス 8 ブロック)*/

/*=================================================================
 * FW へ npages ページ渡す手順を始める(p に覚える)。渡すのは mlx5_pages_step()。
 * 区画の残りが足りなければ BFM_E_NOMEM(何も渡さない)。npages <= 0 なら何もしない。
 * ===============================================================*/
int mlx5_pages_start(bfm_dev_t *d, bfm_pages_t *p, uint16_t func, int32_t npages)
{
    memset(p, 0, sizeof(*p));
    p->func = func;
    if (npages <= 0) return BFM_OK;
    if ((uint32_t)npages > g_bfm_fw_pages_max - d->fw_pages_used) {
        BFM_LOG("bfm: FW が %d ページ求めたが、残りは %u ページ。g_bfm_fw_pages_max(今 %u)を"
                "少なくとも %u に増やすこと\n", (int)npages, g_bfm_fw_pages_max - d->fw_pages_used,
                g_bfm_fw_pages_max, d->fw_pages_used + (uint32_t)npages);
        return BFM_E_NOMEM;
    }
    p->left = (uint32_t)npages;
    return BFM_OK;
}

/*=================================================================
 * MANAGE_PAGES(GIVE)で、残りのページを 512 ページずつ渡す(待たずに進める)。
 * ページは DMA 領域の中の FW 用の区画(g_bfm_fw_pages_max ページ)から順に出す。
 * 返してもらうことはしない。p->cant が立っていれば、代わりに CANT_GIVE を 1 回伝える。
 * 戻り値: BFM_INPROGRESS = まだ残りがある。BFM_OK = 渡し終えた。負 = 失敗。
 * ===============================================================*/
int mlx5_pages_step(bfm_dev_t *d, bfm_pages_t *p)
{
    static uint8_t in[16u + PAGES_PER_CMD * 8u];
    uint8_t out[16];
    if (p->cant) {
        /* 渡せないと伝えないと FW は要求を出し直し続ける */
        uint8_t cin[16] = {0};
        bfm_wr16be(&cin[0], MLX5_CMD_MANAGE_PAGES);
        bfm_wr16be(&cin[10], p->func);
        const int rc = mlx5_cmd_run(d, &p->cmd, cin, sizeof(cin), out, sizeof(out), "MANAGE_PAGES(CANT_GIVE)");
        if (rc == BFM_INPROGRESS) return rc;
        p->cant = 0;
        return rc;
    }
    while (p->left > 0) {
        if (!p->cmd.posted) {
            p->n = (p->left > PAGES_PER_CMD) ? PAGES_PER_CMD : p->left;
            memset(in, 0, 16);
            bfm_wr16be(&in[0], MLX5_CMD_MANAGE_PAGES);
            bfm_wr16be(&in[6], 1u);                       /* op_mod = GIVE */
            bfm_wr16be(&in[10], p->func);
            bfm_wr32be(&in[12], p->n);
            for (uint32_t i = 0; i < p->n; i++) {
                const uint64_t pa = bfm_dma_addr(d->fw_pages + (uint64_t)(d->fw_pages_used + i) * BFM_PAGE);
                bfm_wr64be(&in[16u + i * 8u], pa);
            }
        }
        const int rc = mlx5_cmd_run(d, &p->cmd, in, 16u + p->n * 8u, out, sizeof(out), "MANAGE_PAGES(GIVE)");
        if (rc != BFM_OK) return rc;
        d->fw_pages_used += p->n;
        p->left -= p->n;
    }
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * EQ
 * ------------------------------------------------------------------- */

#define EQE_SIZE        64u
#define EQ_DB_NOARM     0x48u    /* UAR ページの中の EQ のドアベル(arm しない)*/

static bfm_pages_t s_eq_pages;   /* EQ で受けたページ要求に答えている途中の手順 */
static int s_eq_pages_busy;      /* s_eq_pages が動いている */
static struct {
    int      valid;              /* 答えている間に次の要求が来た */
    uint16_t func;               /* その要求の function */
    int32_t  n;                  /* その要求のページの数 */
} s_eq_next;                     /* 答え終えたら答える要求(来た中で最後のもの)*/

/* EQ を作る前に、ページ要求に答える手順を空にする(mlx5_hca_init_step() から)。 */
void mlx5_eq_reset(void)
{
    memset(&s_eq_pages, 0, sizeof(s_eq_pages));
    memset(&s_eq_next, 0, sizeof(s_eq_next));
    s_eq_pages_busy = 0;
}

/* ページ要求に答える手順を始める(区画が足りなければ CANT_GIVE を伝える手順にする)。 */
static void eq_pages_begin(bfm_dev_t *d, uint16_t func, int32_t n)
{
    if (n > 0) {
        if (mlx5_pages_start(d, &s_eq_pages, func, n) != BFM_OK) s_eq_pages.cant = 1;
        s_eq_pages_busy = 1;
    } else if (n < 0) {
        BFM_DBG("bfm: FW がページの返却を申し出た(%d)。預けたままにする\n", (int)n);
    }
}

/*=================================================================
 * EQ のリングを DMA 領域から切り出して所有の印を初期値にし、購読するイベントを決める
 * (Linux の MLX5_ASYNC_EVENT_MASK と同じ組 + ページ要求)。コマンドは出さない。
 * ===============================================================*/
int mlx5_eq_prepare(bfm_dev_t *d)
{
    const uint32_t nent = 1u << BFM_EQ_LOG;
    const uint32_t bytes = nent * EQE_SIZE;
    if ((bytes + BFM_PAGE - 1u) / BFM_PAGE > 4u) return BFM_E_PARAM;   /* CREATE_EQ に載る PAS は 4 ページまで */
    d->eq_buf = (volatile uint8_t *)bfm_mem_alloc(bytes, BFM_PAGE);
    if (d->eq_buf == NULL) return BFM_E_NOMEM;
    for (uint32_t i = 0; i < nent; i++) d->eq_buf[i * EQE_SIZE + EQE_SIZE - 1u] = 1u;  /* owner の初期値 */
    d->eq_mask = (1ull << MLX5_EVT_PATH_MIG) | (1ull << MLX5_EVT_COMM_EST) |
                 (1ull << MLX5_EVT_SQ_DRAINED) | (1ull << MLX5_EVT_CQ_ERROR) |
                 (1ull << MLX5_EVT_WQ_CATAS_ERROR) | (1ull << MLX5_EVT_PATH_MIG_FAILED) |
                 (1ull << MLX5_EVT_WQ_INVAL_REQ) | (1ull << MLX5_EVT_WQ_ACCESS_ERROR) |
                 (1ull << MLX5_EVT_PORT_CHANGE) | (1ull << MLX5_EVT_SRQ_CATAS_ERROR) |
                 (1ull << MLX5_EVT_SRQ_LAST_WQE) | (1ull << MLX5_EVT_SRQ_RQ_LIMIT);
    if (!d->page_request_disable) d->eq_mask |= 1ull << MLX5_EVT_PAGE_REQUEST;
    return BFM_OK;
}

/*=================================================================
 * EQ を 1 本作るコマンド(CREATE_EQ)を進める。割り込みは使わない(arm しない)。
 * 中身は mlx5_eq_poll() で読む。EQ のリング(mlx5_eq_prepare())と UAR(d->eq_uarn)は
 * 呼び出し側が先に用意する。
 * ===============================================================*/
int mlx5_eq_create(bfm_dev_t *d, bfm_cmd_t *j)
{
    static uint8_t in[272 + 8 * 4];
    const uint32_t npages = ((1u << BFM_EQ_LOG) * EQE_SIZE + BFM_PAGE - 1u) / BFM_PAGE;
    uint8_t out[16];
    if (!j->posted) {
        memset(in, 0, sizeof(in));
        bfm_wr16be(&in[0], MLX5_CMD_CREATE_EQ);
        uint8_t *eqc = &in[16];
        eqc[12] = (uint8_t)BFM_EQ_LOG;                /* log_eq_size */
        bfm_wr24be(&eqc[13], d->eq_uarn);             /* uar_page */
        eqc[24] = 0;                                  /* log_page_size = 4KiB */
        bfm_wr64be(&in[88], d->eq_mask);              /* event_bitmask[0] */
        for (uint32_t p = 0; p < npages; p++) bfm_wr64be(&in[272 + p * 8u], bfm_dma_addr(d->eq_buf + p * BFM_PAGE));
    }
    const int rc = mlx5_cmd_run(d, j, in, 272u + npages * 8u, out, sizeof(out), "CREATE_EQ");
    if (rc != BFM_OK) return rc;
    d->eqn = out[11];
    d->eq_cc = 0;
    d->eq_ready = 1;
    return BFM_OK;
}

static void mlx5_eq_update_ci(bfm_dev_t *d)
{
    bfm_mmio_wr32be(d->bar0, (uint64_t)d->eq_uarn * BFM_PAGE + EQ_DB_NOARM,
                    (d->eq_cc & 0xffffffu) | (d->eqn << 24));
}

/*=================================================================
 * EQ に溜まったイベントを読み、ページ要求に答える手順を進める。ページ要求は、1 回の
 * 読み取りの最後の要求にだけ答える(FW は答えが来るまで同じ要求を出し直すので、全部に
 * 答えると与えすぎる。vfio_nvme で実機確認済み)。答えている途中に来た要求は、来た中で
 * 最後のものを覚えておき、答え終えてから答える。エラーのイベントは接続をエラーにする。
 * 戻り値: 読んだイベントの数。
 * ===============================================================*/
int mlx5_eq_poll(bfm_dev_t *d)
{
    if (!d->eq_ready) return 0;
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
        if (!s_eq_pages_busy) {
            eq_pages_begin(d, page_func, page_n);
        } else {
            /* 答えている間に来た要求は、答え終えてから答える。受け流すと FW は出し直さず、渡すページが
             * 前の版(読み取りごとに最後の要求に答えていた)より少なくなった(ConnectX-4 Lx で確認) */
            s_eq_next.valid = 1;
            s_eq_next.func = page_func;
            s_eq_next.n = page_n;
        }
    }
    if (s_eq_pages_busy) {
        const int rc = mlx5_pages_step(d, &s_eq_pages);
        if (rc != BFM_INPROGRESS) {
            s_eq_pages_busy = 0;
            if (rc == BFM_OK) BFM_DBG("bfm: FW のページ要求に答えた(計 %u ページ)\n", d->fw_pages_used);
            if (s_eq_next.valid) {
                s_eq_next.valid = 0;
                eq_pages_begin(d, s_eq_next.func, s_eq_next.n);
            }
        }
    }
    return n;
}

/* 1 = EQ で受けたページ要求に答えている途中。 */
int mlx5_eq_busy(void)
{
    return s_eq_pages_busy;
}
