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
 * 接続・面ごとに、署名つきの MKey 1 個と PSV 2 個(メモリ側・ワイヤ側)をポートの立ち上げで作っておく。
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

#include <cpuid.h>
#include <immintrin.h>
#include <string.h>

#define BLK    BFM_DIF_BLOCK
#define PIB    BFM_DIF_PI
#define BLK_PI (BFM_DIF_BLOCK + BFM_DIF_PI)

/* ---------------------------------------------------------------------
 * CRC16(T10-DIF: 多項式 P = x^16 + 0x8BB7、初期値 0、反転なし。CRC(M) = M(x)·x^16 mod P)
 *
 * 2 つの方法を持ち、CPU が PCLMULQDQ と SSSE3 を持っていれば(かつ g_bfm_dif_clmul が 1 なら)速いほうを使う。
 *   表: 8 バイトずつ表を引く(slice-by-8)。どの x86-64 でも使える。
 *   PCLMULQDQ: 16 バイトを 128 ビットの多項式として読み(バイト順を逆にして、先頭のバイトの最上位ビットを x^127 に)、
 *     キャリーなし乗算で畳み込む(Intel の「Fast CRC Computation for Generic Polynomials Using PCLMULQDQ」、
 *     Linux の crct10dif-pclmul と同じ考え)。X·x^D ≡ Xh·(x^(D+64) mod P) ⊕ Xl·(x^D mod P)(Xh・Xl は X の上下 64 ビット)
 *     で、乗算 2 回で 128 ビットに戻る。4 本の並びを 64 バイトずつ同時に進め(D = 512)、最後に 1 本にまとめ、
 *     64 ビットまで縮めてから表を 8 バイト分引いて CRC にする。64 バイトより短いもの・16 で割った余りは表で続ける。
 * ------------------------------------------------------------------- */
/* 表: s_crc_tab[k][n] = バイト n の後に 0 が k バイト続くときの CRC。 */
static uint16_t s_crc_tab[8][256];
/* 畳み込みの定数(x^n mod P)。添字は n / 64(n = 64, 128, ..., 576)。 */
static uint64_t s_crc_k[10];
static int s_crc_use_clmul;                 /* 1 = PCLMULQDQ の方法を使う */

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

/* x^n mod P(16 ビット未満の多項式)。 */
static uint64_t xpow_mod(uint32_t n)
{
    uint32_t r = 1u;                        /* x^0 */
    for (uint32_t i = 0; i < n; i++) {
        r <<= 1;
        if (r & 0x10000u) r ^= 0x18bb7u;
    }
    return r;
}

/* CPU が PCLMULQDQ(CPUID 1 の ecx[1])と SSSE3(ecx[9])を持つか。CPUID はコンパイラの <cpuid.h> で読む(OS は使わない)。 */
static int cpu_has_clmul(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    return (c & bit_PCLMUL) && (c & bit_SSSE3);
}

/* 表と定数を一度だけ作り、方法を決める(複数のコアから同時に呼ばれてもよいように、作り終えたことを旗で知らせる)。 */
static uint32_t s_crc_ready;                /* 作り終えた(acquire / release で読み書きする)*/
static bfm_lock_t s_crc_lock;               /* 作るのを 1 つのコアだけにする鍵 */

static void crc_ready(void)
{
    if (__atomic_load_n(&s_crc_ready, __ATOMIC_ACQUIRE)) return;
    bfm_lock(&s_crc_lock);
    if (!s_crc_ready) {
        crc_table_init();
        for (uint32_t i = 1; i < 10u; i++) s_crc_k[i] = xpow_mod(i * 64u);
        s_crc_use_clmul = (g_bfm_dif_clmul != 0) && cpu_has_clmul();
        __atomic_store_n(&s_crc_ready, 1u, __ATOMIC_RELEASE);
    }
    bfm_unlock(&s_crc_lock);
}

/* 表: crc から続けて p の len バイトを進める。 */
static uint32_t crc16_tab_upd(uint32_t crc, const uint8_t *p, uint32_t len)
{
    for (; len >= 8u; len -= 8u, p += 8) {
        crc = (uint32_t)s_crc_tab[7][p[0] ^ (crc >> 8)] ^ s_crc_tab[6][p[1] ^ (crc & 0xffu)] ^
              s_crc_tab[5][p[2]] ^ s_crc_tab[4][p[3]] ^ s_crc_tab[3][p[4]] ^ s_crc_tab[2][p[5]] ^
              s_crc_tab[1][p[6]] ^ s_crc_tab[0][p[7]];
    }
    for (; len > 0; len--, p++) crc = ((crc << 8) ^ s_crc_tab[0][((crc >> 8) ^ *p) & 0xffu]) & 0xffffu;
    return crc;
}

/* X(128 ビット)を D ビット先へ畳み込む: X·x^D ≡ Xh·kh ⊕ Xl·kl(k の上の 64 ビット = x^(D+64) mod P、下 = x^D mod P)。 */
__attribute__((target("pclmul,ssse3")))
static inline __m128i crc_fold(__m128i x, __m128i k)
{
    return _mm_xor_si128(_mm_clmulepi64_si128(x, k, 0x11), _mm_clmulepi64_si128(x, k, 0x00));
}

/* PCLMULQDQ: p の len バイト(64 以上)の CRC。16 で割り切れる分を畳み込み、残りは表で続ける。 */
__attribute__((target("pclmul,ssse3")))
static uint16_t crc16_clmul(const uint8_t *p, uint32_t len)
{
    const __m128i bswap = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
#define CRC_LOAD(q) _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(const void *)(q)), bswap)
    const __m128i k512 = _mm_set_epi64x((long long)s_crc_k[9], (long long)s_crc_k[8]);   /* x^576, x^512 */
    const __m128i k384 = _mm_set_epi64x((long long)s_crc_k[7], (long long)s_crc_k[6]);   /* x^448, x^384 */
    const __m128i k256 = _mm_set_epi64x((long long)s_crc_k[5], (long long)s_crc_k[4]);   /* x^320, x^256 */
    const __m128i k128 = _mm_set_epi64x((long long)s_crc_k[3], (long long)s_crc_k[2]);   /* x^192, x^128 */
    const __m128i k64 = _mm_set_epi64x(0, (long long)s_crc_k[1]);                        /* x^64 */
    uint32_t n = len / 16u;
    __m128i a0 = CRC_LOAD(p), a1 = CRC_LOAD(p + 16), a2 = CRC_LOAD(p + 32), a3 = CRC_LOAD(p + 48);
    p += 64;
    n -= 4u;
    for (; n >= 4u; n -= 4u, p += 64) {       /* 4 本を 64 バイトずつ進める */
        a0 = _mm_xor_si128(crc_fold(a0, k512), CRC_LOAD(p));
        a1 = _mm_xor_si128(crc_fold(a1, k512), CRC_LOAD(p + 16));
        a2 = _mm_xor_si128(crc_fold(a2, k512), CRC_LOAD(p + 32));
        a3 = _mm_xor_si128(crc_fold(a3, k512), CRC_LOAD(p + 48));
    }
    /* 1 本にまとめる: a0·x^384 ⊕ a1·x^256 ⊕ a2·x^128 ⊕ a3 */
    __m128i x = _mm_xor_si128(_mm_xor_si128(crc_fold(a0, k384), crc_fold(a1, k256)),
                              _mm_xor_si128(crc_fold(a2, k128), a3));
    for (; n > 0u; n--, p += 16) x = _mm_xor_si128(crc_fold(x, k128), CRC_LOAD(p));   /* 残りの 16 バイトずつ */
#undef CRC_LOAD
    /* 128 ビット -> 80 ビット未満 -> 64 ビット(どれも X と mod P で等しい)*/
    __m128i t = _mm_xor_si128(_mm_clmulepi64_si128(x, k64, 0x01), _mm_move_epi64(x));
    t = _mm_xor_si128(_mm_clmulepi64_si128(t, k64, 0x01), _mm_move_epi64(t));
    const uint64_t z = (uint64_t)_mm_cvtsi128_si64(t);
    /* CRC = Z·x^16 mod P = Z の 8 バイト(上から)を表で進めたもの。続けて 16 で割った余りのバイトを進める */
    uint8_t zb[8];
    for (int i = 0; i < 8; i++) zb[i] = (uint8_t)(z >> (56 - 8 * i));
    return (uint16_t)crc16_tab_upd(crc16_tab_upd(0, zb, 8u), p, len % 16u);
}

static uint16_t crc16(const uint8_t *p, uint32_t len)
{
    crc_ready();
    if (s_crc_use_clmul && len >= 64u) return crc16_clmul(p, len);
    return (uint16_t)crc16_tab_upd(0, p, len);
}

/* 試験用(bfmem_test crc): 使う方法での CRC、表だけでの CRC、PCLMULQDQ を使っているか。 */
uint16_t bfm_dif_crc16(const void *p, uint32_t len)
{
    return crc16((const uint8_t *)p, len);
}

uint16_t bfm_dif_crc16_table(const void *p, uint32_t len)
{
    crc_ready();
    return (uint16_t)crc16_tab_upd(0, (const uint8_t *)p, len);
}

int bfm_dif_crc_clmul(void)
{
    crc_ready();
    return s_crc_use_clmul;
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
    int in_pi;                       /* 入る側に PI がある */
    int out_pi;                      /* 出る側に PI がある */
    uint32_t in_bs;                  /* 入る側の 1 ブロックのバイト数(520 か 512)*/
    uint32_t out_bs;                 /* 出る側の 1 ブロックのバイト数(520 か 512)*/
    uint16_t in_app;                 /* 入る側の app tag(検査に使う)*/
    uint16_t out_app;                /* 出る側の app tag(作る PI に入れる)*/
    uint32_t in_ref;                 /* 入る側の最初のブロックの ref tag(検査に使う。ブロックごとに 1 足す)*/
    uint32_t out_ref;                /* 出る側の最初のブロックの ref tag(作る PI に入れる)*/
    uint8_t  check;                  /* 入る側の PI で検査するもの(BFM_DIF_CHK_*。0 = 検査しない)*/
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
    uint32_t mkey;                   /* 署名つき MKey(UMR で面のバッファに張る)*/
    uint32_t psv_mem;                /* メモリ側(自分の面)の PI の検査・作成の状態を持つ PSV */
    uint32_t psv_wire;               /* ワイヤ側(相手のメモリ)の PI の検査・作成の状態を持つ PSV */
    uint32_t sigerr_count;           /* 次の UMR に入れる sigerr の印 = この値の下位 1 ビット */
    uint8_t  used;                   /* 張ったまま(次に張る前に空きに戻す)*/
};
static struct sig_res s_sig[BFM_MAX_CONN][BFM_DIF_SLOTS];   /* 接続・面ごと */
static int s_hw[BFM_MAX_PORT];              /* ポートの署名のオフロード(BFM_DIF_HW)の資源を用意できた */

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
    bfm_ops_ctrl(c, w, MLX5_OPCODE_UMR, 16u, c->dev->umr_fence, s->mkey);
    b[16] = 0x80u | 0x20u;                                   /* MLX5_FLAGS_INLINE | MLX5_FLAGS_CHECK_FREE */
    bfm_wr16be(&b[20], 4u);                                  /* xlt_octowords */
    bfm_wr16be(&b[22], 4u);                                  /* bsf_octowords */
    bfm_wr64be(&b[24], SIG_MKEY_MASK);

    b[66] = 0x80u | 0x08u | 0x04u | 0x02u;                   /* UMR_EN | LW | LR | access_mode = KLM */
    bfm_wr32be(&b[68], (s->mkey & 0xffu) | 0xffffff00u);
    bfm_wr32be(&b[76], (1u << 24) | ((s->sigerr_count & 1u) << 26) | (1u << 30) | c->dev->pdn);
    bfm_wr64be(&b[88], (uint64_t)k * (mem_pi ? BLK_PI : BLK));   /* len */
    bfm_wr32be(&b[96], 4u);                                  /* bsfs_octo_size */
    bfm_wr32be(&b[116], 4u);                                 /* xlt_oct_size */

    if (!mem_pi) {
        bfm_wr32be(&b[128], k * BLK);                        /* KLM: bcount key va */
        bfm_wr32be(&b[132], c->dev->lkey);
        bfm_wr64be(&b[136], va);
    } else {
        bfm_wr32be(&b[128], BLK_PI);                         /* bcount_per_cycle */
        bfm_wr32be(&b[132], 0x400u);                         /* op = MLX5_STRIDE_BLOCK_OP */
        bfm_wr32be(&b[136], k);                              /* repeat_count */
        bfm_wr16be(&b[142], 2u);                             /* num_entries */
        bfm_wr16be(&b[144], BLK_PI);                         /* データ: stride bcount key va */
        bfm_wr16be(&b[146], BLK);
        bfm_wr32be(&b[148], c->dev->lkey);
        bfm_wr64be(&b[152], va);
        bfm_wr16be(&b[160], BLK_PI);                         /* PI */
        bfm_wr16be(&b[162], PIB);
        bfm_wr32be(&b[164], c->dev->lkey);
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
    bfm_ops_build_rdma(c, w, is_read, off, c->dev->lkey, va, len, 0);
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
    uint8_t  state;                  /* JOB_IDLE / JOB_RUN / JOB_DONE */
    uint8_t  is_read;                /* 1 = read(相手から読む)、0 = write */
    uint8_t  hw;                     /* 1 = 署名のオフロード(BFM_DIF_HW)で運ぶ */
    struct plan p;                   /* 入る側・出る側の形 */
    uint64_t off;                    /* 相手の公開メモリの中の位置(最初のブロック)*/
    const uint8_t *src;              /* write の元(呼び出し側のバッファ)*/
    uint8_t *dst;                    /* read の先(呼び出し側のバッファ)*/
    uint32_t n, per;                 /* 全体のブロック数、1 面に入るブロック数 */
    uint32_t next;                   /* 次に出すブロック */
    uint32_t issued, completed;      /* 出した分・仕上げた分の数(面 = 数 % BFM_DIF_SLOTS)*/
    int      rc;                     /* 最初の失敗(BFM_OK でなければ、もう出さない)*/
    bfm_dif_err_t err;               /* 見つけた誤りの中身(rc が BFM_E_DIF のとき)*/
    struct piece pc[BFM_DIF_SLOTS];  /* 面ごとの、出してある分 */
};
static struct job s_job[BFM_MAX_CONN] __attribute__((aligned(64)));   /* 接続ごとの DIF の転送の状態 */

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
/* 立ち上げの状態(bfm_dif_init_step() が進める。ポートごと)。 */
enum { D_START = 0, D_PSV, D_MKEY, D_DONE };

static struct {
    int step;                        /* 今の状態(D_*)*/
    unsigned i, si;                  /* 作っている接続の枠(ポートの中の番号)と面 */
    uint8_t key8;                    /* 署名つき MKey の鍵の下位 8 ビットの起点 */
    bfm_cmd_t cmd;                   /* 出しているコマンド */
} s_di[BFM_MAX_PORT];

/* 立ち上げの状態を最初に戻す(ポートの立ち上げを始めるときに呼ぶ)。 */
void bfm_dif_init_reset(bfm_dev_t *d)
{
    memset(&s_di[d->port], 0, sizeof(s_di[d->port]));
}

/*=================================================================
 * ポート d の T10-DIF の準備を、待つところまで進める。CRC の表を作り、装置に署名オフロード(sho)が
 * あれば、そのポートの接続の枠・面ごとに PSV 2 つと署名つき MKey を作る(コマンドの完了を待つところで
 * BFM_INPROGRESS を返す)。作れなければ表示して BFM_DIF_SW だけにする(失敗にはしない)。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。
 * ===============================================================*/
int bfm_dif_init_step(bfm_dev_t *d)
{
    for (;;) {
        switch (s_di[d->port].step) {
        case D_START:
            for (unsigned i = 0; i < d->nconn; i++) {
                const int k = d->conn[i].index;
                memset(s_sig[k], 0, sizeof(s_sig[k]));
                memset(&s_job[k], 0, sizeof(s_job[k]));
            }
            s_hw[d->port] = 0;
            crc_ready();                   /* 表は立ち上げのときに作っておく */
            if (!d->sho) {
                BFM_DBG("bfm: ポート %u: 署名のオフロード(sho)が無い。T10-DIF は BFM_DIF_SW だけ\n", d->port);
                s_di[d->port].step = D_DONE;
                break;
            }
            s_di[d->port].key8 = (uint8_t)(bfm_port_time_ns() >> 12);
            s_di[d->port].i = s_di[d->port].si = 0;
            s_di[d->port].step = D_PSV;
            break;
        case D_PSV:
        case D_MKEY: {
            const unsigned i = s_di[d->port].i, si = s_di[d->port].si;
            struct sig_res *s = &s_sig[d->conn[i].index][si];
            int rc;
            if (s_di[d->port].step == D_PSV) {
                uint32_t psv[2];
                rc = mlx5_create_psv2(d, &s_di[d->port].cmd, psv);
                if (rc == BFM_OK) {
                    s->psv_mem = psv[0];
                    s->psv_wire = psv[1];
                }
            } else {
                rc = mlx5_create_sig_mkey(d, &s_di[d->port].cmd,
                                          (uint8_t)(s_di[d->port].key8 + i * BFM_DIF_SLOTS + si), &s->mkey);
            }
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) {
                BFM_LOG("bfm: ポート %u: 注意: 署名の資源を作れない。T10-DIF は BFM_DIF_SW だけ\n", d->port);
                s_di[d->port].step = D_DONE;        /* 作った分は TEARDOWN_HCA で消える */
                break;
            }
            if (s_di[d->port].step == D_PSV) {
                s_di[d->port].step = D_MKEY;
                break;
            }
            s->sigerr_count = 1;           /* Linux と同じく、最初の UMR で sigerr の印を立てる */
            s_di[d->port].step = D_PSV;
            if (++s_di[d->port].si == BFM_DIF_SLOTS) {
                s_di[d->port].si = 0;
                if (++s_di[d->port].i == d->nconn) {
                    s_hw[d->port] = 1;
                    BFM_DBG("bfm: ポート %u: T10-DIF のオフロードを使える\n", d->port);
                    s_di[d->port].step = D_DONE;
                }
            }
            break;
        }
        default:   /* D_DONE */
            return BFM_OK;
        }
    }
}

/* 接続ができたとき: 転送の状態を空にし、面ごとの署名つき MKey を poll_one() に教える。 */
void bfm_dif_attach(struct bfm_conn *c)
{
    memset(&s_job[c->index], 0, sizeof(s_job[c->index]));
    memset(c->sig, 0, sizeof(c->sig));
    for (unsigned si = 0; si < BFM_DIF_SLOTS; si++) c->sig_mkey[si] = s_sig[c->index][si].mkey;
}

/* 使う全部のポートで BFM_DIF_HW の資源を用意できたときだけ 1。 */
int bfm_dif_hw_supported(void)
{
    int any = 0;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        if (!g_bfm_dev[p].used) continue;
        if (!s_hw[p]) return 0;
        any = 1;
    }
    return any;
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
    if (d->mode == BFM_DIF_HW && !s_hw[c->dev->port]) return BFM_E_NOTSUP;
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
