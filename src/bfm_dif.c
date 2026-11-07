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
 * 接続・面ごとに、署名つきの MKey 1 個と PSV 2 個(メモリ側・ワイヤ側)を bfm_init() で作っておく。
 * 中継バッファの面(メモリ側の形)を、ブロックの形を書いた BSF つきで MKey に張り、その MKey を
 * 手元のバッファとして RDMA を出すと、NIC が通る途中で PI を検査・付け替えする。
 *   1. (前に使っていれば)UMR で MKey を空きに戻す(Linux の LOCAL_INV)
 *   2. UMR: MKey に、中継バッファの位置(KLM か 520 バイトおきの strided block)と BSF を入れる
 *   3. SET_PSV x 2: メモリ側・ワイヤ側の PSV に、最初のブロックの app tag / ref tag を入れる
 *   4. RDMA WRITE / READ(手元 = 署名つき MKey のアドレス 0 から、fence = INITIATOR_SMALL)
 * 検査で誤りがあると、その RDMA の完了の前に SIG_ERR の CQE が来る(bfm_ops.c の poll_one())。
 * 誤りを 1 回見たら、次の UMR で MKey の sigerr の印を反転させる(Linux の sigerr_count と同じ)。
 *
 * 起動(bfm_dif_write_start() / bfm_dif_read_start())と完了確認(bfm_dif_poll() / bfm_dif_wait())を
 * 分けてある。DIF 用の中継バッファ(接続ごと)を 2 面に分け、1 回の RDMA で 1 面に入るブロック数ずつ
 * 運ぶ。2 面までを同時に出しておき、片方を NIC が運ぶ間に、もう片方を CPU が用意・検査する。
 * HW では面ごとに署名つき MKey と PSV を持ち、SIG_ERR の CQE の MKey でどちらの面の誤りかを見分ける。
 * 転送が進むのは bfm_dif_poll() / bfm_dif_wait() の中だけ(割り込みは使わない)。 */
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

/* 表を一度だけ作る(複数のコアから同時に呼ばれてもよいように、作り終えたことを旗で知らせる)。 */
static uint32_t s_crc_ready;
static bfm_lock_t s_crc_lock;

static void crc_ready(void)
{
    if (__atomic_load_n(&s_crc_ready, __ATOMIC_ACQUIRE)) return;
    bfm_lock(&s_crc_lock);
    if (!s_crc_ready) {
        crc_table_init();
        __atomic_store_n(&s_crc_ready, 1u, __ATOMIC_RELEASE);
    }
    bfm_unlock(&s_crc_lock);
}

static uint16_t crc16(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0;
    crc_ready();
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

/* ---------------------------------------------------------------------
 * HW: 署名オフロード
 * ------------------------------------------------------------------- */
struct sig_res {
    uint32_t mkey;
    uint32_t psv_mem, psv_wire;
    uint32_t sigerr_count;           /* 次の UMR に入れる sigerr の印 = この値の下位 1 ビット */
    uint8_t  used;                   /* 張ったまま(次に張る前に空きに戻す)*/
};
static struct sig_res s_sig[BFM_MAX_CONN][BFM_DIF_SLOTS];   /* 接続・面ごと */
static int s_hw;

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
 * k はブロック数、first は全体の中での最初のブロックの番号、va は中継バッファの面の先頭。
 * ===============================================================*/
static void build_umr(struct bfm_conn *c, struct sig_res *s, uint64_t *w, const struct plan *p,
                      int is_read, uint32_t k, uint32_t first, uint64_t va)
{
    const int mem_pi = is_read ? p->out_pi : p->in_pi;
    const int wire_pi = is_read ? p->in_pi : p->out_pi;
    const uint16_t mem_app = is_read ? p->out_app : p->in_app;
    const uint16_t wire_app = is_read ? p->in_app : p->out_app;
    const uint32_t mem_ref = (is_read ? p->out_ref : p->in_ref) + first;
    const uint32_t wire_ref = (is_read ? p->in_ref : p->out_ref) + first;
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
 * HW: 中継バッファの面 si の k ブロック(メモリ側の形、先頭 va)と、相手のメモリ + off
 * (ワイヤ側の形)の間で、署名オフロードを通して運ぶ WQE を出す(待たない)。
 * first は全体の中での最初のブロックの番号。面ごとに署名つき MKey と PSV を持つので、
 * 2 面を同時に出しておける。
 * ===============================================================*/
static int hw_post(struct bfm_conn *c, uint32_t si, const struct plan *p, int is_read, uint64_t off,
                   uint32_t k, uint32_t first, uint64_t va)
{
    struct sig_res *s = &s_sig[c->index][si];
    uint64_t w[BFM_WQE_MAX / 8u];
    int rc;
    const int mem_pi = is_read ? p->out_pi : p->in_pi;
    const int wire_pi = is_read ? p->in_pi : p->out_pi;

    if (s->used) {
        build_linv(c, s, w);
        if ((rc = bfm_ops_post(c, w, 2u)) != BFM_OK) return rc;
        s->used = 0;
    }
    build_umr(c, s, w, p, is_read, k, first, va);
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
    return bfm_ops_post(c, w, 1u);
}

/* SW: 中継バッファの面(先頭 va)と相手のメモリ + off の間で len バイトを運ぶ RDMA を出す(待たない)。 */
static int sw_post(struct bfm_conn *c, int is_read, uint64_t off, uint64_t va, uint32_t len)
{
    uint64_t w[8];
    bfm_ops_build_rdma(c, w, is_read, off, g_bfm.lkey, va, len, 0);
    return bfm_ops_post(c, w, 1u);
}

/* ---------------------------------------------------------------------
 * 転送の進め方(接続ごとに 1 本)
 *
 * 全体を「1 面に入るブロック数」ずつの分に切り、分 i を面 i % 2 で運ぶ。出しておく分は 2 つまで。
 *   出す(issue()):     write は src から面へ用意して(SW は検査・PI の付け外し、HW は写すだけ)
 *                       RDMA を出す。read は面へ読む RDMA を出す。
 *   仕上げる(finish()): その分の RDMA が済んだら(CQE は出した順に来る)、HW は署名の誤りを見る。
 *                       read は面から dst へ(SW は検査・PI の付け外し、HW は写すだけ)。
 * 誤りを見つけたら、それ以上は出さず、出してある分が済むのを待ってから終わる。
 * ------------------------------------------------------------------- */
enum { JOB_IDLE = 0, JOB_RUN = 1, JOB_DONE = 2 };   /* DONE = 終わって、結果を受け取られていない */

struct piece {
    uint32_t first, k;               /* この分の最初のブロック(全体の番号)とブロック数 */
    uint32_t end_pc;                 /* この分の最後の WQE の次の位置(sq_done がここまで来たら済み)*/
};

struct __attribute__((aligned(64))) job {   /* 接続(= コア)ごと。隣とキャッシュラインを分ける */
    uint8_t  state, is_read, hw;
    struct plan p;
    uint64_t off;
    const uint8_t *src;
    uint8_t *dst;
    uint32_t n, per;                 /* 全体のブロック数、1 面に入るブロック数 */
    uint32_t next;                   /* 次に出すブロック */
    uint32_t issued, completed;      /* 出した分・仕上げた分の数(面 = 数 % BFM_DIF_SLOTS)*/
    int      rc;                     /* 最初の失敗(BFM_OK でなければ、もう出さない)*/
    bfm_dif_err_t err;
    struct piece pc[BFM_DIF_SLOTS];
};
static struct job s_job[BFM_MAX_CONN] __attribute__((aligned(64)));

static inline uint8_t *slot_buf(const struct bfm_conn *c, uint32_t si)
{
    return (uint8_t *)(uintptr_t)(c->dif_buf + (uint64_t)si * c->dif_slot_size);
}

/* 次の分を用意して出す。 */
static int issue(struct bfm_conn *c, struct job *j)
{
    const uint32_t si = j->issued % BFM_DIF_SLOTS;
    struct piece *pc = &j->pc[si];
    uint8_t *buf = slot_buf(c, si);
    const struct plan *p = &j->p;
    int rc;

    pc->first = j->next;
    pc->k = (j->n - j->next < j->per) ? j->n - j->next : j->per;
    if (!j->is_read) {
        const uint8_t *src = j->src + (uint64_t)pc->first * p->in_bs;
        if (j->hw) {
            memcpy(buf, src, (size_t)pc->k * p->in_bs);
        } else {
            for (uint32_t i = 0; i < pc->k; i++) {
                rc = sw_block(p, src + (uint64_t)i * p->in_bs, buf + i * p->out_bs, pc->first + i, &j->err);
                if (rc != BFM_OK) return rc;     /* この分は送らない */
            }
        }
    }
    const uint32_t remote_bs = j->is_read ? p->in_bs : p->out_bs;
    const uint64_t roff = j->off + (uint64_t)pc->first * remote_bs;
    rc = j->hw ? hw_post(c, si, p, j->is_read, roff, pc->k, pc->first, bfm_dma_addr(buf))
               : sw_post(c, j->is_read, roff, bfm_dma_addr(buf), pc->k * remote_bs);
    if (rc != BFM_OK) return rc;
    pc->end_pc = c->qp.sq_pc;
    j->next += pc->k;
    j->issued++;
    return BFM_OK;
}

/* 済んだ分を仕上げる。前に失敗していれば、署名の誤りの後始末だけをする。 */
static int finish(struct bfm_conn *c, struct job *j)
{
    const uint32_t si = j->completed % BFM_DIF_SLOTS;
    const struct piece *pc = &j->pc[si];
    const uint8_t *buf = slot_buf(c, si);
    const struct plan *p = &j->p;
    int rc = BFM_OK;

    j->completed++;
    if (j->hw) {
        uint32_t ok = pc->k;                   /* dst へ写してよいブロック数 */
        struct bfm_sig_rec *r = &c->sig[si];
        if (r->hits != 0) {
            /* 誤りを見たら、次の UMR で MKey の sigerr の印を反転させる(Linux の sigerr_count と同じ)。
             * 位置は署名つき MKey の中のバイトで来るので、PI つきのブロック(520)で割る(Linux の iSER と同じ)。 */
            s_sig[c->index][si].sigerr_count += r->hits;
            uint32_t b = (uint32_t)(r->off / BLK_PI);
            if (b >= pc->k) b = pc->k - 1u;
            ok = b;
            if (j->rc == BFM_OK) {
                if (r->synd & (1u << 13)) {
                    set_err(&j->err, pc->first + b, BFM_DIF_CHK_GUARD, r->exp_ts >> 16, r->act_ts >> 16);
                } else if (r->synd & (1u << 11)) {
                    set_err(&j->err, pc->first + b, BFM_DIF_CHK_REF, r->exp_ref, r->act_ref);
                } else {
                    set_err(&j->err, pc->first + b, BFM_DIF_CHK_APP, r->exp_ts & 0xffffu, r->act_ts & 0xffffu);
                }
                rc = BFM_E_DIF;
            }
            memset(r, 0, sizeof(*r));
        }
        if (j->rc == BFM_OK && j->is_read && ok > 0)
            memcpy(j->dst + (uint64_t)pc->first * p->out_bs, buf, (size_t)ok * p->out_bs);
        return rc;
    }
    if (j->rc != BFM_OK || !j->is_read) return BFM_OK;
    for (uint32_t i = 0; i < pc->k; i++) {
        rc = sw_block(p, buf + i * p->in_bs, j->dst + (uint64_t)(pc->first + i) * p->out_bs,
                      pc->first + i, &j->err);
        if (rc != BFM_OK) return rc;
    }
    return BFM_OK;
}

/* 来ている完了を拾い、済んだ分を仕上げ、空いた面で次の分を出す。待たない。 */
static void progress(struct bfm_conn *c, struct job *j)
{
    int rc = bfm_ops_reap(c);
    if (rc != BFM_OK || c->state != BFM_CS_UP) {
        /* 接続がエラーになった。出してある分はもう済まない */
        if (j->rc == BFM_OK) j->rc = (rc != BFM_OK) ? rc : BFM_E_STATE;
        j->state = JOB_DONE;
        return;
    }
    while (j->completed < j->issued &&
           (int32_t)(c->sq_done - j->pc[j->completed % BFM_DIF_SLOTS].end_pc) >= 0) {
        rc = finish(c, j);
        if (rc != BFM_OK && j->rc == BFM_OK) j->rc = rc;
    }
    while (j->rc == BFM_OK && j->next < j->n && j->issued - j->completed < BFM_DIF_SLOTS) {
        rc = issue(c, j);
        if (rc != BFM_OK) j->rc = rc;
    }
    if (j->completed == j->issued && (j->rc != BFM_OK || j->next == j->n)) j->state = JOB_DONE;
}

/* ---------------------------------------------------------------------
 * 立ち上げ
 * ------------------------------------------------------------------- */
int bfm_dif_init(void)
{
    memset(s_sig, 0, sizeof(s_sig));
    memset(s_job, 0, sizeof(s_job));
    s_hw = 0;
    crc_ready();                           /* 表は立ち上げのときに作っておく */
    if (!g_bfm.sho) {
        BFM_DBG("bfm: 署名のオフロード(sho)が無い。T10-DIF は BFM_DIF_SW だけ\n");
        return BFM_OK;
    }
    const uint8_t key8 = (uint8_t)(bfm_port_time_ns() >> 12);
    for (unsigned i = 0; i < g_bfm.nconn; i++) {
        for (unsigned si = 0; si < BFM_DIF_SLOTS; si++) {
            struct sig_res *s = &s_sig[i][si];
            uint32_t psv[2];
            if (mlx5_create_psv2(&g_bfm, psv) != BFM_OK ||
                mlx5_create_sig_mkey(&g_bfm, (uint8_t)(key8 + i * BFM_DIF_SLOTS + si), &s->mkey) != BFM_OK) {
                BFM_LOG("bfm: 注意: 署名の資源を作れない。T10-DIF は BFM_DIF_SW だけ\n");
                return BFM_OK;             /* 作った分は TEARDOWN_HCA で消える */
            }
            s->psv_mem = psv[0];
            s->psv_wire = psv[1];
            s->sigerr_count = 1;           /* Linux と同じく、最初の UMR で sigerr の印を立てる */
        }
    }
    s_hw = 1;
    BFM_DBG("bfm: T10-DIF のオフロードを使える(接続 0 の面 0: MKey 0x%08x PSV %u / %u)\n",
            s_sig[0][0].mkey, s_sig[0][0].psv_mem, s_sig[0][0].psv_wire);
    return BFM_OK;
}

/* 接続ができたとき: 転送の状態を空にし、面ごとの署名つき MKey を poll_one() に教える。 */
void bfm_dif_attach(struct bfm_conn *c)
{
    memset(&s_job[c->index], 0, sizeof(s_job[c->index]));
    memset(c->sig, 0, sizeof(c->sig));
    for (unsigned si = 0; si < BFM_DIF_SLOTS; si++) c->sig_mkey[si] = s_sig[c->index][si].mkey;
}

int bfm_dif_hw_supported(void)
{
    return s_hw;
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
    const uint32_t remote_bs = is_read ? p->in_bs : p->out_bs;
    rc = bfm_ops_check_range(c, off, (uint64_t)n * remote_bs);
    if (rc != BFM_OK) return rc;
    return (c->dif_slot_size < BLK_PI) ? BFM_E_PARAM : BFM_OK;
}

/* 転送を起動する(最初の分を、2 面まで出す)。 */
static int start(struct bfm_conn *c, uint64_t off, const void *buf, uint32_t n, const bfm_dif_t *d,
                 int is_read)
{
    struct plan p;
    const int rc = prepare(c, off, buf, n, d, is_read, &p);
    if (rc != BFM_OK) return rc;
    struct job *j = &s_job[c->index];
    if (j->state != JOB_IDLE) return BFM_E_BUSY;
    memset(j, 0, sizeof(*j));
    j->p = p;
    j->is_read = (uint8_t)is_read;
    j->hw = (uint8_t)(d->mode == BFM_DIF_HW);
    j->off = off;
    j->src = (const uint8_t *)buf;
    j->dst = (uint8_t *)(uintptr_t)buf;
    j->n = n;
    j->per = c->dif_slot_size / BLK_PI;
    j->state = JOB_RUN;
    progress(c, j);                    /* ここでの失敗も、結果として bfm_dif_poll() が返す */
    return BFM_OK;
}

int bfm_dif_write_start(bfm_conn_t *c, uint64_t off, const void *src, uint32_t nblocks,
                        const bfm_dif_t *dif)
{
    return start(c, off, src, nblocks, dif, 0);
}

int bfm_dif_read_start(bfm_conn_t *c, uint64_t off, void *dst, uint32_t nblocks,
                       const bfm_dif_t *dif)
{
    return start(c, off, dst, nblocks, dif, 1);
}

int bfm_dif_poll(bfm_conn_t *c, bfm_dif_err_t *err)
{
    if (c == NULL) return BFM_E_PARAM;
    struct job *j = &s_job[c->index];
    if (j->state == JOB_RUN) progress(c, j);
    if (j->state == JOB_RUN) return BFM_DIF_INPROGRESS;
    if (j->state == JOB_IDLE) return BFM_OK;
    j->state = JOB_IDLE;                 /* 結果を返すのは 1 回だけ */
    if (j->rc == BFM_E_DIF && err != NULL) *err = j->err;
    return j->rc;
}

int bfm_dif_wait(bfm_conn_t *c, bfm_dif_err_t *err)
{
    if (c == NULL) return BFM_E_PARAM;
    uint64_t t0 = bfm_port_time_ns();
    uint32_t last = c->sq_done, spins = 0;
    for (;;) {
        const int rc = bfm_dif_poll(c, err);
        if (rc != BFM_DIF_INPROGRESS) return rc;
        if ((++spins & 0xfffu) != 0) continue;
        const uint64_t now = bfm_port_time_ns();
        if (c->sq_done != last) {        /* 進んでいれば測り直す */
            last = c->sq_done;
            t0 = now;
        } else if (now - t0 > (uint64_t)g_bfm_op_timeout_ms * 1000000u) {
            BFM_LOG("bfm: 接続 %d: T10-DIF の転送が %u ms 進まない\n", c->index, g_bfm_op_timeout_ms);
            c->state = BFM_CS_ERROR;
            s_job[c->index].state = JOB_IDLE;
            return BFM_E_TIMEOUT;
        }
    }
}

int bfm_dif_write(bfm_conn_t *c, uint64_t off, const void *src, uint32_t nblocks,
                  const bfm_dif_t *dif, bfm_dif_err_t *err)
{
    const int rc = bfm_dif_write_start(c, off, src, nblocks, dif);
    return (rc != BFM_OK) ? rc : bfm_dif_wait(c, err);
}

int bfm_dif_read(bfm_conn_t *c, uint64_t off, void *dst, uint32_t nblocks,
                 const bfm_dif_t *dif, bfm_dif_err_t *err)
{
    const int rc = bfm_dif_read_start(c, off, dst, nblocks, dif);
    return (rc != BFM_OK) ? rc : bfm_dif_wait(c, err);
}
