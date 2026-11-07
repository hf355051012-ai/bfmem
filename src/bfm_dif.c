/* T10-DIF(512 + 8 バイトのブロック)をつけた転送: bfm_dif_write() / bfm_dif_read()。
 *
 * どちらも、入る側の形のブロックを出る側の形に変えて運ぶ(形と操作は bfm.h)。
 * write は自分のメモリが入る側・相手のメモリが出る側、read はその反対。
 *
 * BFM_DIF_SW(オフロードなし): CPU が中継バッファで形を変える。
 *   write: src -> (検査・PI の付け外し)-> 中継バッファ -> RDMA WRITE
 *   read : RDMA READ -> 中継バッファ -> (検査・PI の付け外し)-> dst
 *
 * BFM_DIF_HW(オフロードあり): NIC の署名オフロード(Linux の IB_WR_REG_MR_INTEGRITY と同じ手順)。
 * 接続ごとに、署名つきの MKey 1 個と PSV 2 個(メモリ側・ワイヤ側)を bfm_init() で作っておく。
 * 中継バッファ(メモリ側の形)を、ブロックの形を書いた BSF つきで MKey に張り、その MKey を
 * 手元のバッファとして RDMA を出すと、NIC が通る途中で PI を検査・付け替えする。
 *   1. (前に使っていれば)UMR で MKey を空きに戻す(Linux の LOCAL_INV)
 *   2. UMR: MKey に、中継バッファの位置(KLM か 520 バイトおきの strided block)と BSF を入れる
 *   3. SET_PSV x 2: メモリ側・ワイヤ側の PSV に、最初のブロックの app tag / ref tag を入れる
 *   4. RDMA WRITE / READ(手元 = 署名つき MKey のアドレス 0 から、fence = INITIATOR_SMALL)
 * 検査で誤りがあると、その RDMA の完了の前に SIG_ERR の CQE が来る(bfm_ops.c の poll_one())。
 * 誤りを 1 回見たら、次の UMR で MKey の sigerr の印を反転させる(Linux の sigerr_count と同じ)。
 *
 * 1 回の RDMA で運ぶのは、中継バッファに入るブロック数まで(足りなければ繰り返す)。 */
#include "bfm_internal.h"

#include <string.h>

#define BLK    BFM_DIF_BLOCK
#define PIB    BFM_DIF_PI
#define BLK_PI (BFM_DIF_BLOCK + BFM_DIF_PI)

/* ---------------------------------------------------------------------
 * CRC16(T10-DIF: 多項式 0x8BB7、初期値 0、反転なし)
 * ------------------------------------------------------------------- */
/* 8 バイトずつ表を引く(slice-by-8)。s_crc_tab[k][n] = バイト n の後に 0 が k バイト続くときの CRC。 */
static uint16_t s_crc_tab[8][256];

static void crc_table_init(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000u) ? (((uint32_t)c << 1) ^ 0x8bb7u) : ((uint32_t)c << 1));
        s_crc_tab[0][i] = c;
    }
    for (uint32_t k = 1; k < 8u; k++) {
        for (uint32_t i = 0; i < 256u; i++) {
            const uint16_t c = s_crc_tab[k - 1u][i];
            s_crc_tab[k][i] = (uint16_t)((uint32_t)(c << 8) ^ s_crc_tab[0][c >> 8]);
        }
    }
}

static uint16_t crc16(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0;
    if (s_crc_tab[0][1] == 0) crc_table_init();
    for (; len >= 8u; len -= 8u, p += 8) {
        crc = (uint32_t)s_crc_tab[7][p[0] ^ (crc >> 8)] ^ s_crc_tab[6][p[1] ^ (crc & 0xffu)] ^
              s_crc_tab[5][p[2]] ^ s_crc_tab[4][p[3]] ^ s_crc_tab[3][p[4]] ^ s_crc_tab[2][p[5]] ^
              s_crc_tab[1][p[6]] ^ s_crc_tab[0][p[7]];
    }
    for (; len > 0; len--, p++) crc = ((crc << 8) ^ s_crc_tab[0][((crc >> 8) ^ *p) & 0xffu]) & 0xffffu;
    return (uint16_t)crc;
}

void bfm_dif_make_pi(const void *data, uint16_t app_tag, uint32_t ref_tag, uint8_t pi[8])
{
    bfm_wr16be(&pi[0], crc16((const uint8_t *)data, BLK));
    bfm_wr16be(&pi[2], app_tag);
    bfm_wr32be(&pi[4], ref_tag);
}

/* ---------------------------------------------------------------------
 * 操作を「入る側・出る側」の形に直したもの
 * ------------------------------------------------------------------- */
struct plan {
    int in_pi, out_pi;               /* 入る側・出る側に PI があるか */
    uint32_t in_bs, out_bs;          /* 1 ブロックのバイト数(520 か 512)*/
    uint16_t in_app, out_app;
    uint32_t in_ref, out_ref;        /* 最初のブロックの ref tag */
    uint8_t  check;
};

static int make_plan(const bfm_dif_t *d, struct plan *p)
{
    memset(p, 0, sizeof(*p));
    p->in_app = p->out_app = d->app_tag;
    p->in_ref = p->out_ref = d->ref_tag;
    p->check = (uint8_t)(d->check & BFM_DIF_CHK_ALL);
    switch (d->op) {
    case BFM_DIF_CHECK:        p->in_pi = 1; p->out_pi = 1; break;
    case BFM_DIF_CHECK_SWAP:   p->in_pi = 1; p->out_pi = 1;
                               p->out_app = d->new_app_tag; p->out_ref = d->new_ref_tag; break;
    case BFM_DIF_INSERT:       p->in_pi = 0; p->out_pi = 1; p->check = 0; break;
    case BFM_DIF_CHECK_DELETE: p->in_pi = 1; p->out_pi = 0; break;
    default: return BFM_E_PARAM;
    }
    p->in_bs = p->in_pi ? BLK_PI : BLK;
    p->out_bs = p->out_pi ? BLK_PI : BLK;
    return BFM_OK;
}

static void set_err(bfm_dif_err_t *e, uint32_t block, uint8_t what, uint32_t expected, uint32_t actual)
{
    if (e == NULL) return;
    e->block = block;
    e->what = what;
    e->expected = expected;
    e->actual = actual;
}

/*=================================================================
 * SW: 1 ブロックを入る側の形 in から出る側の形 out に変える。i は先頭からの番号。
 * 出る側の PI の各欄は、NIC の署名オフロードと同じ決め方にする: 入る側に PI があり、
 * 値が同じ(app tag・ref tag)なら入る側のバイトをそのまま写し、違えば新しい値を書く。
 * guard は入る側にあれば写し、無ければ計算する。
 * ===============================================================*/
static int sw_block(const struct plan *p, const uint8_t *in, uint8_t *out, uint32_t i, bfm_dif_err_t *e)
{
    const uint8_t *pi = in + BLK;
    uint16_t guard = 0;
    int have_guard = 0;
    if (p->in_pi && p->check) {
        if (p->check & BFM_DIF_CHK_GUARD) {
            guard = crc16(in, BLK);
            have_guard = 1;
            if (bfm_rd16be(&pi[0]) != guard) {
                set_err(e, i, BFM_DIF_CHK_GUARD, guard, bfm_rd16be(&pi[0]));
                return BFM_E_DIF;
            }
        }
        if ((p->check & BFM_DIF_CHK_REF) && bfm_rd32be(&pi[4]) != p->in_ref + i) {
            set_err(e, i, BFM_DIF_CHK_REF, p->in_ref + i, bfm_rd32be(&pi[4]));
            return BFM_E_DIF;
        }
        if ((p->check & BFM_DIF_CHK_APP) && bfm_rd16be(&pi[2]) != p->in_app) {
            set_err(e, i, BFM_DIF_CHK_APP, p->in_app, bfm_rd16be(&pi[2]));
            return BFM_E_DIF;
        }
    }
    if (out != in) memcpy(out, in, BLK);
    if (p->out_pi) {
        uint8_t *po = out + BLK;
        if (p->in_pi) {
            memcpy(&po[0], &pi[0], 2);
        } else {
            bfm_wr16be(&po[0], have_guard ? guard : crc16(in, BLK));
        }
        if (p->in_pi && p->out_app == p->in_app) memcpy(&po[2], &pi[2], 2);
        else bfm_wr16be(&po[2], p->out_app);
        if (p->in_pi && p->out_ref == p->in_ref) memcpy(&po[4], &pi[4], 4);
        else bfm_wr32be(&po[4], p->out_ref + i);
    }
    return BFM_OK;
}

/* 中継バッファと相手のメモリの間で len バイトを運ぶ(1 回、終わるまで待つ)。 */
static int stage_rdma(struct bfm_conn *c, int is_read, uint64_t off, uint32_t len)
{
    uint64_t w[8];
    bfm_ops_build_rdma(c, w, is_read, off, g_bfm.lkey, bfm_dma_addr(c->stage), len, 0);
    const int rc = bfm_ops_post(c, w, 1u);
    return (rc != BFM_OK) ? rc : bfm_ops_wait_all(c);
}

static int sw_write(struct bfm_conn *c, const struct plan *p, uint64_t off, const uint8_t *src,
                    uint32_t n, bfm_dif_err_t *e)
{
    const uint32_t per = c->stage_size / BLK_PI;
    uint8_t *st = (uint8_t *)(uintptr_t)c->stage;
    for (uint32_t done = 0; done < n;) {
        const uint32_t k = (n - done < per) ? n - done : per;
        for (uint32_t i = 0; i < k; i++) {
            const int rc = sw_block(p, src + (uint64_t)(done + i) * p->in_bs, st + i * p->out_bs, done + i, e);
            if (rc != BFM_OK) return rc;
        }
        const int rc = stage_rdma(c, 0, off + (uint64_t)done * p->out_bs, k * p->out_bs);
        if (rc != BFM_OK) return rc;
        done += k;
    }
    return BFM_OK;
}

static int sw_read(struct bfm_conn *c, const struct plan *p, uint64_t off, uint8_t *dst, uint32_t n,
                   bfm_dif_err_t *e)
{
    const uint32_t per = c->stage_size / BLK_PI;
    const uint8_t *st = (const uint8_t *)(uintptr_t)c->stage;
    for (uint32_t done = 0; done < n;) {
        const uint32_t k = (n - done < per) ? n - done : per;
        int rc = stage_rdma(c, 1, off + (uint64_t)done * p->in_bs, k * p->in_bs);
        if (rc != BFM_OK) return rc;
        for (uint32_t i = 0; i < k; i++) {
            rc = sw_block(p, st + i * p->in_bs, dst + (uint64_t)(done + i) * p->out_bs, done + i, e);
            if (rc != BFM_OK) return rc;
        }
        done += k;
    }
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * HW: 署名オフロード
 * ------------------------------------------------------------------- */
struct sig_res {
    uint32_t mkey;
    uint32_t psv_mem, psv_wire;
    uint32_t sigerr_count;           /* 次の UMR に入れる sigerr の印 = この値の下位 1 ビット */
    uint8_t  used;                   /* 張ったまま(次に張る前に空きに戻す)*/
};
static struct sig_res s_sig[BFM_MAX_CONN];
static int s_hw;

int bfm_dif_init(void)
{
    memset(s_sig, 0, sizeof(s_sig));
    s_hw = 0;
    if (!g_bfm.sho) {
        BFM_DBG("bfm: 署名のオフロード(sho)が無い。T10-DIF は BFM_DIF_SW だけ\n");
        return BFM_OK;
    }
    const uint8_t key8 = (uint8_t)(bfm_port_time_ns() >> 12);
    for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
        uint32_t psv[2];
        if (mlx5_create_psv2(&g_bfm, psv) != BFM_OK ||
            mlx5_create_sig_mkey(&g_bfm, (uint8_t)(key8 + i), &s_sig[i].mkey) != BFM_OK) {
            BFM_LOG("bfm: 注意: 署名の資源を作れない。T10-DIF は BFM_DIF_SW だけ\n");
            return BFM_OK;                 /* 作った分は TEARDOWN_HCA で消える */
        }
        s_sig[i].psv_mem = psv[0];
        s_sig[i].psv_wire = psv[1];
        s_sig[i].sigerr_count = 1;         /* Linux と同じく、最初の UMR で sigerr の印を立てる */
    }
    s_hw = 1;
    BFM_DBG("bfm: T10-DIF のオフロードを使える(接続 0: MKey 0x%08x PSV %u / %u)\n",
            s_sig[0].mkey, s_sig[0].psv_mem, s_sig[0].psv_wire);
    return BFM_OK;
}

int bfm_dif_hw_supported(void)
{
    return s_hw;
}

/* mkey_mask(Linux の sig_mkey_mask()): LEN PAGE_SIZE START_ADDR EN_RINVAL EN_SIGERR BSF_EN KEY
 * LR LW RR RW SMALL_FENCE FREE */
#define SIG_MKEY_MASK ((1ull << 0) | (1ull << 1) | (1ull << 6) | (1ull << 8) | (1ull << 9) | \
                       (1ull << 12) | (1ull << 13) | (1ull << 17) | (1ull << 18) | (1ull << 19) | \
                       (1ull << 20) | (1ull << 23) | (1ull << 29))
#define MKEY_MASK_FREE (1ull << 29)

/* BSF の inline の部分(16 バイト。Linux の mlx5_fill_inl_bsf())。 */
static void bsf_inl(uint8_t *p, uint16_t app, uint32_t ref)
{
    bfm_wr16be(&p[0], 0xc000u);            /* 有効 | DIF を毎回入れ直す(REFRESH)*/
    bfm_wr16be(&p[2], app);
    bfm_wr32be(&p[4], ref);
    p[8] = 0x01u;                          /* guard = CRC */
    p[9] = 0x80u;                          /* ブロックの繰り返し */
    p[13] = 0x40u;                         /* ref tag をブロックごとに 1 増やす */
    bfm_wr16be(&p[14], 0xffffu);           /* app tag は全ビットを比べる */
}

/*=================================================================
 * 署名つき MKey を張る UMR の WQE(4 WQEBB = 256 バイト)を組む。
 *   [0..15]    制御(UMR、imm = MKey)
 *   [16..63]   UMR の制御: flags(inline | 空きを確かめる)・変換 4 octword・BSF 4 octword・mkey_mask
 *   [64..127]  MKey: KLM・LR / LW / UMR_EN、BSF あり、sigerr の印、長さ = メモリ側のバイト数
 *   [128..191] 変換: メモリ側に PI が無ければ KLM 1 個、あれば 520 バイトおきの strided block
 *              (データ 512 と PI 8 の 2 項目)
 *   [192..255] BSF: basic・ext(0)・ワイヤ側の inline・メモリ側の inline
 * k はブロック数、mem_* / wire_* はそれぞれの側の PI の有無と最初のブロックの tag。
 * ===============================================================*/
static void build_umr(struct bfm_conn *c, struct sig_res *s, uint64_t *w, const struct plan *p,
                      int is_read, uint32_t k, uint32_t first)
{
    const int mem_pi = is_read ? p->out_pi : p->in_pi;
    const int wire_pi = is_read ? p->in_pi : p->out_pi;
    const uint16_t mem_app = is_read ? p->out_app : p->in_app;
    const uint16_t wire_app = is_read ? p->in_app : p->out_app;
    const uint32_t mem_ref = (is_read ? p->out_ref : p->in_ref) + first;
    const uint32_t wire_ref = (is_read ? p->in_ref : p->out_ref) + first;
    const uint64_t va = bfm_dma_addr(c->stage);
    uint8_t *b = (uint8_t *)w;

    memset(w, 0, 256);
    bfm_ops_ctrl(c, w, MLX5_OPCODE_UMR, 16u, g_bfm.umr_fence, s->mkey);
    b[16] = 0x80u | 0x20u;                                   /* MLX5_FLAGS_INLINE | MLX5_FLAGS_CHECK_FREE */
    bfm_wr16be(&b[20], 4u);                                  /* xlt_octowords */
    bfm_wr16be(&b[22], 4u);                                  /* bsf_octowords */
    bfm_wr64be(&b[24], SIG_MKEY_MASK);

    b[66] = 0x80u | 0x08u | 0x04u | 0x02u;                   /* UMR_EN | LW | LR | access_mode = KLM */
    bfm_wr32be(&b[68], (s->mkey & 0xffu) | 0xffffff00u);
    bfm_wr32be(&b[76], (1u << 24) | ((s->sigerr_count & 1u) << 26) | (1u << 30) | g_bfm.pdn);
    bfm_wr64be(&b[88], (uint64_t)k * (mem_pi ? BLK_PI : BLK));   /* len */
    bfm_wr32be(&b[96], 4u);                                  /* bsfs_octo_size */
    bfm_wr32be(&b[116], 4u);                                 /* xlt_oct_size */

    if (!mem_pi) {
        bfm_wr32be(&b[128], k * BLK);                        /* KLM: bcount key va */
        bfm_wr32be(&b[132], g_bfm.lkey);
        bfm_wr64be(&b[136], va);
    } else {
        bfm_wr32be(&b[128], BLK_PI);                         /* bcount_per_cycle */
        bfm_wr32be(&b[132], 0x400u);                         /* op = MLX5_STRIDE_BLOCK_OP */
        bfm_wr32be(&b[136], k);                              /* repeat_count */
        bfm_wr16be(&b[142], 2u);                             /* num_entries */
        bfm_wr16be(&b[144], BLK_PI);                         /* データ: stride bcount key va */
        bfm_wr16be(&b[146], BLK);
        bfm_wr32be(&b[148], g_bfm.lkey);
        bfm_wr64be(&b[152], va);
        bfm_wr16be(&b[160], BLK_PI);                         /* PI */
        bfm_wr16be(&b[162], PIB);
        bfm_wr32be(&b[164], g_bfm.lkey);
        bfm_wr64be(&b[168], va + BLK);
    }

    uint8_t *f = &b[192];
    f[0] = 0x80u;                                            /* basic + ext + inline */
    if (p->in_pi) {                                          /* 入る側で検査するバイト */
        f[1] = (uint8_t)(((p->check & BFM_DIF_CHK_GUARD) ? 0xc0u : 0u) |
                         ((p->check & BFM_DIF_CHK_APP) ? 0x30u : 0u) |
                         ((p->check & BFM_DIF_CHK_REF) ? 0x0fu : 0u));
    }
    if (mem_pi && wire_pi) {                                 /* 同じブロックの形: 同じ値のバイトは写す */
        f[0] |= 0x10u;
        f[2] = (uint8_t)(0xc0u | (mem_app == wire_app ? 0x30u : 0u) | (mem_ref == wire_ref ? 0x0fu : 0u));
    } else if (wire_pi) {
        f[2] = 0x01u;                                        /* ワイヤ側のブロック = 512 */
    }
    if (mem_pi) f[3] = 0x01u;                                /* メモリ側のブロック = 512 */
    bfm_wr32be(&f[4], k * BLK);                              /* raw_data_size(データだけ)*/
    if (wire_pi) {
        bfm_wr32be(&f[8], s->psv_wire);
        bsf_inl(&f[32], wire_app, wire_ref);
    }
    if (mem_pi) {
        bfm_wr32be(&f[12], s->psv_mem);
        bsf_inl(&f[48], mem_app, mem_ref);
    }
}

/* 署名つき MKey を空きに戻す UMR(2 WQEBB。Linux の LOCAL_INV)。 */
static void build_linv(struct bfm_conn *c, const struct sig_res *s, uint64_t *w)
{
    uint8_t *b = (uint8_t *)w;
    memset(w, 0, 128);
    bfm_ops_ctrl(c, w, MLX5_OPCODE_UMR, 8u, 0, s->mkey);
    b[16] = 0x80u;                                           /* inline */
    bfm_wr64be(&b[24], MKEY_MASK_FREE);
    b[64] = 0x40u;                                           /* status = 空き */
}

/* SET_PSV(1 WQEBB): PSV に最初のブロックの app tag / ref tag を入れる(PI の無い側は 0)。 */
static void build_psv(struct bfm_conn *c, uint64_t *w, uint32_t psv, int pi, uint16_t app, uint32_t ref)
{
    uint8_t *b = (uint8_t *)w;
    memset(w, 0, 64);
    bfm_ops_ctrl(c, w, MLX5_OPCODE_SET_PSV, 2u, (uint8_t)(MLX5_WQE_CTRL_SOLICITED | MLX5_FENCE_SMALL), 0);
    bfm_wr32be(&b[16], psv);
    if (pi) {
        bfm_wr32be(&b[24], app);                             /* transient_sig = guard の種 0 << 16 | app */
        bfm_wr32be(&b[28], ref);
    }
}

/*=================================================================
 * HW: 中継バッファの k ブロック(メモリ側の形)と、相手のメモリ + off(ワイヤ側の形)の間で、
 * 署名オフロードを通して 1 回運ぶ。first は全体の中での最初のブロックの番号。
 * ===============================================================*/
static int hw_xfer(struct bfm_conn *c, const struct plan *p, int is_read, uint64_t off, uint32_t k,
                   uint32_t first, bfm_dif_err_t *e)
{
    struct sig_res *s = &s_sig[c->index];
    uint64_t w[BFM_WQE_MAX / 8u];
    int rc;
    const int mem_pi = is_read ? p->out_pi : p->in_pi;
    const int wire_pi = is_read ? p->in_pi : p->out_pi;

    c->sig_err = 0;
    if (s->used) {
        build_linv(c, s, w);
        if ((rc = bfm_ops_post(c, w, 2u)) != BFM_OK) return rc;
        s->used = 0;
    }
    build_umr(c, s, w, p, is_read, k, first);
    if ((rc = bfm_ops_post(c, w, 4u)) != BFM_OK) return rc;
    s->used = 1;
    build_psv(c, w, s->psv_mem, mem_pi, is_read ? p->out_app : p->in_app,
              (is_read ? p->out_ref : p->in_ref) + first);
    if ((rc = bfm_ops_post(c, w, 1u)) != BFM_OK) return rc;
    build_psv(c, w, s->psv_wire, wire_pi, is_read ? p->in_app : p->out_app,
              (is_read ? p->in_ref : p->out_ref) + first);
    if ((rc = bfm_ops_post(c, w, 1u)) != BFM_OK) return rc;
    /* 手元の長さは Linux の rdma_rw_ctx_signature_init() と同じ: データ + (両側に PI があれば PI) */
    const uint32_t len = k * BLK + ((mem_pi && wire_pi) ? k * PIB : 0u);
    bfm_ops_build_rdma(c, w, is_read, off, s->mkey, 0, len, MLX5_FENCE_SMALL);
    if ((rc = bfm_ops_post(c, w, 1u)) != BFM_OK) return rc;
    if ((rc = bfm_ops_wait_all(c)) != BFM_OK) return rc;
    if (!c->sig_err) return BFM_OK;

    /* 誤り。位置は署名つき MKey の中のバイトで来るので、PI つきのブロック(520)で割る(Linux の iSER と同じ)。 */
    s->sigerr_count++;
    c->sig_err = 0;
    const uint32_t blk = first + (uint32_t)(c->sig_off / BLK_PI);
    if (c->sig_synd & (1u << 13)) {
        set_err(e, blk, BFM_DIF_CHK_GUARD, c->sig_exp_ts >> 16, c->sig_act_ts >> 16);
    } else if (c->sig_synd & (1u << 11)) {
        set_err(e, blk, BFM_DIF_CHK_REF, c->sig_exp_ref, c->sig_act_ref);
    } else {
        set_err(e, blk, BFM_DIF_CHK_APP, c->sig_exp_ts & 0xffffu, c->sig_act_ts & 0xffffu);
    }
    return BFM_E_DIF;
}

static int hw_write(struct bfm_conn *c, const struct plan *p, uint64_t off, const uint8_t *src,
                    uint32_t n, bfm_dif_err_t *e)
{
    const uint32_t per = c->stage_size / BLK_PI;
    for (uint32_t done = 0; done < n;) {
        const uint32_t k = (n - done < per) ? n - done : per;
        memcpy((void *)(uintptr_t)c->stage, src + (uint64_t)done * p->in_bs, (size_t)k * p->in_bs);
        const int rc = hw_xfer(c, p, 0, off + (uint64_t)done * p->out_bs, k, done, e);
        if (rc != BFM_OK) return rc;
        done += k;
    }
    return BFM_OK;
}

static int hw_read(struct bfm_conn *c, const struct plan *p, uint64_t off, uint8_t *dst, uint32_t n,
                   bfm_dif_err_t *e)
{
    const uint32_t per = c->stage_size / BLK_PI;
    for (uint32_t done = 0; done < n;) {
        const uint32_t k = (n - done < per) ? n - done : per;
        const int rc = hw_xfer(c, p, 1, off + (uint64_t)done * p->in_bs, k, done, e);
        if (rc != BFM_OK) return rc;
        memcpy(dst + (uint64_t)done * p->out_bs, (const void *)(uintptr_t)c->stage, (size_t)k * p->out_bs);
        done += k;
    }
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * 公開する関数
 * ------------------------------------------------------------------- */

/* 引数を確かめ、操作を入る側・出る側の形に直す。remote_bs は相手のメモリ側のブロックの大きさ。 */
static int prepare(struct bfm_conn *c, uint64_t off, const void *buf, uint32_t n, const bfm_dif_t *d,
                   int is_read, struct plan *p)
{
    if (buf == NULL || d == NULL || n == 0) return BFM_E_PARAM;
    int rc = make_plan(d, p);
    if (rc != BFM_OK) return rc;
    if (d->mode != BFM_DIF_SW && d->mode != BFM_DIF_HW) return BFM_E_PARAM;
    if (d->mode == BFM_DIF_HW && !s_hw) return BFM_E_NOTSUP;
    if (c != NULL && c->stage_size < BLK_PI) return BFM_E_PARAM;
    const uint32_t remote_bs = is_read ? p->in_bs : p->out_bs;
    return bfm_ops_check_range(c, off, (uint64_t)n * remote_bs);
}

int bfm_dif_write(bfm_conn_t *c, uint64_t off, const void *src, uint32_t nblocks,
                  const bfm_dif_t *dif, bfm_dif_err_t *err)
{
    struct plan p;
    const int rc = prepare(c, off, src, nblocks, dif, 0, &p);
    if (rc != BFM_OK) return rc;
    return (dif->mode == BFM_DIF_HW) ? hw_write(c, &p, off, (const uint8_t *)src, nblocks, err)
                                     : sw_write(c, &p, off, (const uint8_t *)src, nblocks, err);
}

int bfm_dif_read(bfm_conn_t *c, uint64_t off, void *dst, uint32_t nblocks,
                 const bfm_dif_t *dif, bfm_dif_err_t *err)
{
    struct plan p;
    const int rc = prepare(c, off, dst, nblocks, dif, 1, &p);
    if (rc != BFM_OK) return rc;
    return (dif->mode == BFM_DIF_HW) ? hw_read(c, &p, off, (uint8_t *)dst, nblocks, err)
                                     : sw_read(c, &p, off, (uint8_t *)dst, nblocks, err);
}
