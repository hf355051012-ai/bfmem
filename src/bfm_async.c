/* 多重度つきの write / read / atomic(完了を待たずに出しておき、後で確かめる)。
 *
 * 1 本の接続で、完了を待たずに最大 g_bfm_qdepth(既定 64。上限 BFM_QD_MAX)個の write / read / atomic を
 * 出しておける。仕掛かり中の 1 個ごとに「枠」を 1 つ使う。枠は、送り元 / 着地点にする中継バッファ
 * (g_bfm_async_buf_size。既定 4KiB)を 1 つずつ持つ。
 *
 *   bfm_write_async(c, off, src, len, BFM_ACK, &tag)    ACK を確かめる write。tag で 1 個ずつ確かめる
 *   bfm_write_async(c, off, src, len, BFM_NOACK, NULL)  ACK を確かめない write(tag なし)
 *   bfm_read_async(c, off, dst, len, &tag)              read。完了を刈り取ったときに dst へ写す
 *   bfm_cas_async / bfm_faa_async / bfm_cas32_async / bfm_faa32_async(c, …, &old, &tag)
 *                                                       atomic。元の値は刈り取ったときに *old へ入れる
 *   bfm_test(c, tag) / bfm_wait(c, tag)                 その 1 個が済んだか(待たない / 済むまで待つ)
 *   bfm_check(c, &inflight)                             完了を刈り取り、時間切れ・エラーを報告する
 *
 * 枠が全部仕掛かり中のときに出そうとすると、CQ から完了(ACK・read の応答)を刈り取って枠を空けてから出す。
 * 空かなければ回って待ち、仕掛かり中のどれかが出してから g_bfm_async_timeout_ms(既定 100ms)を過ぎたら
 * (または待ち始めてからその時間を過ぎたら)、接続をエラーにして BFM_E_TIMEOUT を返す。枠が空いていれば
 * 待たずに出す。
 *
 * 完了の刈り取りは、どの関数で行っても同じ(poll_one() が bfm_async_done() を呼ぶ)。普通の bfm_write()
 * などで完了を待つ間にも刈り取られる。刈り取った枠はすぐ空き(次の操作に使える)、結果は次にその枠を
 * 使うまで残す。tag は「通し番号 << 6 | 枠の番号」で、枠が次の操作に使われた後の古い tag は
 * 「済んで、成功した」とみなす(失敗した操作があると接続がエラーになり、次の操作は出せないので、
 * 枠が使い回されるのは成功の後だけ)。
 *
 * 全部の WQE に完了通知を求める(ACK を確かめない write も。枠を空ける時と時間切れを知るため)。
 * 完了は出した順に来る(RC の SQ)。read / atomic の同時の数は QP の上限(相手が受け付ける数。16 など)で
 * NIC が抑える(超えた分は NIC の中で待つ)。1 つの接続を複数のコアから同時に使わないこと(ほかの
 * データ経路の関数と同じ)。 */
#include "bfm_internal.h"

#include <string.h>

#define SQ_N       (1u << BFM_RC_SQ_LOG)
#define SCAT_MAX   32u                       /* read の応答が CQE に入る長さ(scatter to CQE)*/
#define TAG_SLOT   6u                        /* tag の下位の、枠の番号のビット数 */
#define SEQ_MASK   0x3ffffffu                /* tag の上位 26 ビット(通し番号)*/

static uint64_t timeout_ns(void)
{
    return (uint64_t)g_bfm_async_timeout_ms * 1000000u;
}

/* 接続ができたとき(張り直しを含む): 枠を全部空ける(枠の数と中継バッファは接続の枠を用意したときに
 * 決めてある)。通し番号は張り直しをまたいで続け、ここから進めた数を asince に数える(tag が「何個前に
 * 出したものか」が asince より大きければ、張り直しの前に出した操作で、結果はもう分からない)。 */
void bfm_async_attach(struct bfm_conn *c)
{
    memset(c->as, 0, sizeof(c->as));
    memset(c->wqe_slot, 0, sizeof(c->wqe_slot));
    c->afree = (c->aqd >= 64u) ? ~0ull : ((1ull << c->aqd) - 1u);
    if ((c->aseq & SEQ_MASK) == 0u) c->aseq++;          /* 通し番号 0 は使わない(as[] の空の印)*/
    c->asince = 0;
    c->ainflight = 0;
    c->aerr = 0;
}

/* 枠の操作の名前(表示用)。 */
static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case BFM_AK_READ:     return "read";
    case BFM_AK_WRITE:    return "write";
    case BFM_AK_ATOMIC:   return "atomic(8 バイト)";
    case BFM_AK_ATOMIC32: return "atomic(4 バイト)";
    default:              return "write(ACK を確かめない)";
    }
}

/*=================================================================
 * 位置 pc の WQE の完了を、その枠に入れる(poll_one() から)。成功なら、read は応答を手元の dst へ写し、
 * atomic は元の値を dst へ入れる(32 バイトまでは CQE に入っている = scat、そうでなければ枠の中継バッファ)。
 * atomic の元の値のバイト順(ConnectX-4 Lx で確認。同期版の atomic_result() / atomic32_result() と同じ):
 *   8 バイト: CQE に入ればビッグエンディアン、中継バッファならホスト順(HCA をホスト順に設定してある)
 *   4 バイト: どちらもビッグエンディアン
 * 枠を空きに戻す。
 * ===============================================================*/
void bfm_async_done(struct bfm_conn *c, uint32_t pc, const volatile uint8_t *scat, int rc)
{
    const uint32_t k1 = c->wqe_slot[pc & (SQ_N - 1u)];
    if (k1 == 0u) return;
    c->wqe_slot[pc & (SQ_N - 1u)] = 0u;
    const uint32_t k = k1 - 1u;
    struct bfm_aslot *s = &c->as[k];
    if (!s->busy) return;
    const volatile uint8_t *buf = c->abuf + (uint64_t)k * c->abuf_slot;
    if (rc == BFM_OK && s->kind == BFM_AK_READ) {
        uint8_t *d = (uint8_t *)s->dst;
        if (scat != NULL && s->len <= SCAT_MAX) {
            for (uint32_t i = 0; i < s->len; i++) d[i] = scat[i];
        } else {
            memcpy(d, (const void *)(uintptr_t)buf, s->len);
        }
    } else if (rc == BFM_OK && s->kind == BFM_AK_ATOMIC && s->dst != NULL) {
        uint64_t v;
        if (scat != NULL) v = bfm_rd64be(scat);
        else memcpy(&v, (const void *)(uintptr_t)buf, 8);
        *(uint64_t *)s->dst = v;
    } else if (rc == BFM_OK && s->kind == BFM_AK_ATOMIC32 && s->dst != NULL) {
        *(uint32_t *)s->dst = bfm_rd32be(scat != NULL ? scat : buf);
    }
    s->rc = rc;
    s->busy = 0;
    c->afree |= 1ull << k;
    c->ainflight--;
    if (rc != BFM_OK) {
        if (c->aerr == 0) c->aerr = rc;
        BFM_LOG("bfm: 接続 %d: 非同期の %s(tag 0x%08x、+0x%llx、%u バイト)がエラーで終わった: %s\n", c->index,
                kind_name(s->kind),
                (s->seq << TAG_SLOT) | k, (unsigned long long)s->off, s->len, bfm_strerror(rc));
    }
}

/* 仕掛かり中のうち、出してから時間切れを過ぎた最も古い枠(-1 = 無い)。 */
static int overdue(const struct bfm_conn *c, uint64_t now)
{
    int k = -1;
    uint64_t oldest = now;
    for (uint32_t i = 0; i < c->aqd; i++) {
        if (c->as[i].busy && c->as[i].t < oldest) {
            oldest = c->as[i].t;
            k = (int)i;
        }
    }
    return (k >= 0 && now - oldest > timeout_ns()) ? k : -1;
}

/* 時間切れ: 表示して接続をエラーにする(張り直すまで使えない)。 */
static int fail_timeout(struct bfm_conn *c, int k, const char *why)
{
    const struct bfm_aslot *s = &c->as[k];
    BFM_LOG("bfm: 接続 %d: %s(%u ms)。仕掛かり中の %s(tag 0x%08x、+0x%llx、%u バイト)に ACK・応答が無い。"
            "接続は使えなくなった\n", c->index, why, g_bfm_async_timeout_ms,
            kind_name(s->kind),
            (s->seq << TAG_SLOT) | (uint32_t)k, (unsigned long long)s->off, s->len);
    if (c->state == BFM_CS_UP) c->state = BFM_CS_ERROR;
    if (c->aerr == 0) c->aerr = BFM_E_TIMEOUT;
    return BFM_E_TIMEOUT;
}

/*=================================================================
 * 空いている枠を 1 つ取る(SQ にも bbs 個の WQEBB の空きがあること)。空いていれば待たない。
 * 空いていなければ、完了を刈り取って空くまで回って待つ。仕掛かり中のどれかが出してから時間切れを
 * 過ぎたら、または待ち始めてから時間切れを過ぎたら、BFM_E_TIMEOUT(接続はエラーになる)。
 * ===============================================================*/
static int get_slot(struct bfm_conn *c, uint32_t bbs, uint32_t *slot)
{
    uint64_t t0 = 0;
    uint32_t spins = 0;
    for (;;) {
        if (c->afree != 0 && (c->qp.sq_pc + bbs) - c->sq_done <= SQ_N) {
            *slot = (uint32_t)__builtin_ctzll(c->afree);
            return BFM_OK;
        }
        (void)bfm_ops_reap_all(c);
        if (c->aerr != 0) return c->aerr;
        if (c->state != BFM_CS_UP) return BFM_E_STATE;
        if (c->afree != 0 && (c->qp.sq_pc + bbs) - c->sq_done <= SQ_N) continue;
        if ((++spins & 0xffu) != 0u) {
            bfm_cpu_relax();
            continue;
        }
        const uint64_t now = bfm_port_time_ns();
        if (t0 == 0) t0 = now;
        const int k = overdue(c, now);
        if (k >= 0) return fail_timeout(c, k, "枠の空きを待つ間に時間切れ");
        if (now - t0 > timeout_ns()) {
            BFM_LOG("bfm: 接続 %d: 枠・SQ の空きを %u ms 待っても空かない\n", c->index, g_bfm_async_timeout_ms);
            if (c->state == BFM_CS_UP) c->state = BFM_CS_ERROR;
            if (c->aerr == 0) c->aerr = BFM_E_TIMEOUT;
            return BFM_E_TIMEOUT;
        }
    }
}

/* 出した操作を枠 k に記録する(位置 pc の WQE と枠を結び、通し番号を振って tag を返す)。
 * 出した後(bfm_ops_post() が位置の枠の番号を 0 にした後)に呼ぶ。 */
static void commit(struct bfm_conn *c, uint32_t k, uint32_t pc, uint8_t kind, void *dst, uint32_t len, uint64_t off,
                   bfm_tag_t *tag)
{
    struct bfm_aslot *s = &c->as[k];
    const uint32_t before = c->aseq;
    s->seq = c->aseq++ & SEQ_MASK;
    if (s->seq == 0u) s->seq = c->aseq++ & SEQ_MASK;   /* 26 ビットで回ったとき、0 は飛ばす */
    /* 今の接続になってから進めた数(SEQ_MASK で止める。止まった後は、張り直しの前の tag と見分けない)*/
    c->asince = (c->asince + (c->aseq - before) >= SEQ_MASK) ? SEQ_MASK : c->asince + (c->aseq - before);
    s->busy = 1;
    s->kind = kind;
    s->rc = BFM_INPROGRESS;
    s->dst = dst;
    s->len = len;
    s->off = off;
    s->t = bfm_port_time_ns();
    c->wqe_slot[pc & (SQ_N - 1u)] = (uint8_t)(k + 1u);
    c->afree &= ~(1ull << k);
    c->ainflight++;
    if (tag != NULL) *tag = (s->seq << TAG_SLOT) | k;
}

/*=================================================================
 * 非同期の操作を 1 個出す(write は src を WQE に埋め込むか枠の中継バッファへ写してから出すので、
 * 戻った後に src を変えてよい。read は刈り取ったときに dst へ写す)。tag が NULL でなければ tag を返す。
 * ===============================================================*/
static int issue(struct bfm_conn *c, uint8_t kind, uint64_t off, const void *src, void *dst, uint32_t len,
                 bfm_tag_t *tag)
{
    uint64_t w[BFM_WQE_MAX / 8u];
    int rc = bfm_ops_check_range(c, off, len);
    if (rc != BFM_OK) return rc;
    if (c->aerr != 0) return c->aerr;
    const int inl = (kind != BFM_AK_READ && len <= bfm_inline_max());
    if (!inl && len > c->abuf_slot) return BFM_E_PARAM;
    const uint32_t bbs = inl ? (2u + (4u + len + 15u) / 16u + 3u) / 4u : 1u;
    uint32_t k = 0;
    if ((rc = get_slot(c, bbs, &k)) != BFM_OK) return rc;
    volatile uint8_t *buf = c->abuf + (uint64_t)k * c->abuf_slot;
    const uint32_t pc = c->qp.sq_pc;
    if (inl) {
        (void)bfm_ops_build_write_inline(c, w, off, src, len);
    } else {
        if (kind != BFM_AK_READ) memcpy((void *)(uintptr_t)buf, src, len);
        bfm_ops_build_rdma(c, w, kind == BFM_AK_READ, off, c->dev->lkey, bfm_dma_addr(buf), len, 0);
    }
    if ((rc = bfm_ops_post(c, w, bbs)) != BFM_OK) return rc;
    commit(c, k, pc, kind, dst, len, off, tag);
    return BFM_OK;
}

/*=================================================================
 * 非同期の atomic を 1 個出す(opcode は MLX5_OPCODE_ATOMIC_CS / FA / MASKED_CS / MASKED_FA)。
 * 元の値は枠の中継バッファ(か CQE)に返り、刈り取ったときに old へ入れる(old は NULL でもよい)。
 * ===============================================================*/
static int issue_atomic(struct bfm_conn *c, uint32_t opcode, uint64_t off, uint64_t swap_add, uint64_t cmp,
                        void *old, bfm_tag_t *tag)
{
    uint64_t w[8];
    const int is32 = (opcode == MLX5_OPCODE_ATOMIC_MASKED_CS || opcode == MLX5_OPCODE_ATOMIC_MASKED_FA);
    int rc;
    if (c->aerr != 0) return c->aerr;
    /* 範囲・境界・未対応は、枠の空きを待つ前に断る(一度組んでみる。着地点は枠が決まってから組み直す)*/
    if ((rc = bfm_ops_build_atomic(c, w, opcode, off, swap_add, cmp, 0)) != BFM_OK) return rc;
    uint32_t k = 0;
    if ((rc = get_slot(c, 1u, &k)) != BFM_OK) return rc;
    const uint32_t pc = c->qp.sq_pc;
    rc = bfm_ops_build_atomic(c, w, opcode, off, swap_add, cmp, bfm_dma_addr(c->abuf + (uint64_t)k * c->abuf_slot));
    if (rc != BFM_OK) return rc;
    if ((rc = bfm_ops_post(c, w, 1u)) != BFM_OK) return rc;
    commit(c, k, pc, is32 ? BFM_AK_ATOMIC32 : BFM_AK_ATOMIC, old, is32 ? 4u : 8u, off, tag);
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * 公開する関数
 * ------------------------------------------------------------------- */

int bfm_write_async(bfm_conn_t *c, uint64_t off, const void *src, uint32_t len, int ack, bfm_tag_t *tag)
{
    if (c == NULL || src == NULL || (ack && tag == NULL)) return BFM_E_PARAM;
    return issue(c, ack ? BFM_AK_WRITE : BFM_AK_WRITE_NOACK, off, src, NULL, len, ack ? tag : NULL);
}

int bfm_read_async(bfm_conn_t *c, uint64_t off, void *dst, uint32_t len, bfm_tag_t *tag)
{
    if (c == NULL || dst == NULL || tag == NULL) return BFM_E_PARAM;
    return issue(c, BFM_AK_READ, off, NULL, dst, len, tag);
}

int bfm_test(bfm_conn_t *c, bfm_tag_t tag)
{
    if (c == NULL || (tag & ((1u << TAG_SLOT) - 1u)) >= c->aqd) return BFM_E_PARAM;
    (void)bfm_ops_reap_all(c);
    const uint32_t k = tag & ((1u << TAG_SLOT) - 1u), seq = tag >> TAG_SLOT;
    const struct bfm_aslot *s = &c->as[k];
    if (seq == 0u) return BFM_E_PARAM;
    /* 張り直しより前に出した操作(何個前に出したか = 次の通し番号との差が、今の接続になってから進めた数より大きい):
     * その結果はもう分からない。前は「接続ができたときの番号との差が 26 ビットの半分を超えたら前」と見ていたため、
     * 1 本の接続で約 3,355 万個(2^25)出すと、それ以降の正しい tag まで BFM_E_STATE になっていた */
    const uint32_t age = (c->aseq - seq) & SEQ_MASK;
    if (age == 0u || age > c->asince) return BFM_E_STATE;
    if (s->seq != seq) return BFM_OK;   /* 済んで、枠は次の操作に使われた(= 成功した)*/
    if (!s->busy) return s->rc;
    if (c->state != BFM_CS_UP) return c->aerr ? c->aerr : BFM_E_STATE;   /* 別の操作のエラー・切断 */
    if (bfm_port_time_ns() - s->t > timeout_ns()) return fail_timeout(c, (int)k, "時間切れ");
    return BFM_INPROGRESS;
}

int bfm_wait(bfm_conn_t *c, bfm_tag_t tag)
{
    int rc;
    while ((rc = bfm_test(c, tag)) == BFM_INPROGRESS) bfm_cpu_relax();
    return rc;
}

int bfm_check(bfm_conn_t *c, uint32_t *inflight)
{
    if (c == NULL) return BFM_E_PARAM;
    (void)bfm_ops_reap_all(c);
    if (inflight != NULL) *inflight = c->ainflight;
    if (c->aerr != 0) return c->aerr;
    const int k = overdue(c, bfm_port_time_ns());
    if (k >= 0) return fail_timeout(c, k, "時間切れ");
    if (c->state != BFM_CS_UP) return BFM_E_STATE;
    return BFM_OK;
}

int bfm_cas_async(bfm_conn_t *c, uint64_t off, uint64_t expect, uint64_t desired, uint64_t *old, bfm_tag_t *tag)
{
    if (c == NULL || tag == NULL) return BFM_E_PARAM;
    return issue_atomic(c, MLX5_OPCODE_ATOMIC_CS, off, desired, expect, old, tag);
}

int bfm_faa_async(bfm_conn_t *c, uint64_t off, uint64_t add, uint64_t *old, bfm_tag_t *tag)
{
    if (c == NULL || tag == NULL) return BFM_E_PARAM;
    return issue_atomic(c, MLX5_OPCODE_ATOMIC_FA, off, add, 0, old, tag);
}

int bfm_cas32_async(bfm_conn_t *c, uint64_t off, uint32_t expect, uint32_t desired, uint32_t *old, bfm_tag_t *tag)
{
    if (c == NULL || tag == NULL) return BFM_E_PARAM;
    return issue_atomic(c, MLX5_OPCODE_ATOMIC_MASKED_CS, off, desired, expect, old, tag);
}

int bfm_faa32_async(bfm_conn_t *c, uint64_t off, uint32_t add, uint32_t *old, bfm_tag_t *tag)
{
    if (c == NULL || tag == NULL) return BFM_E_PARAM;
    return issue_atomic(c, MLX5_OPCODE_ATOMIC_MASKED_FA, off, add, 0, old, tag);
}
