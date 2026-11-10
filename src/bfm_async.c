/* 多重度つきの write / read(完了を待たずに出しておき、後で確かめる)。
 *
 * 1 本の接続で、完了を待たずに最大 g_bfm_qdepth(既定 64。上限 BFM_QD_MAX)個の write / read を
 * 出しておける。仕掛かり中の 1 個ごとに「枠」を 1 つ使う。枠は、送り元 / 着地点にする中継バッファ
 * (g_bfm_async_buf_size。既定 4KiB)を 1 つずつ持つ。
 *
 *   bfm_write_async(c, off, src, len, BFM_ACK, &tag)    ACK を確かめる write。tag で 1 個ずつ確かめる
 *   bfm_write_async(c, off, src, len, BFM_NOACK, NULL)  ACK を確かめない write(tag なし)
 *   bfm_read_async(c, off, dst, len, &tag)              read。完了を刈り取ったときに dst へ写す
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
 * 決めてある)。通し番号は張り直しをまたいで続け、ここからの番号を abase に覚える(それより前の tag は、
 * 張り直しの前に出した操作で、結果はもう分からない)。 */
void bfm_async_attach(struct bfm_conn *c)
{
    memset(c->as, 0, sizeof(c->as));
    memset(c->wqe_slot, 0, sizeof(c->wqe_slot));
    c->afree = (c->aqd >= 64u) ? ~0ull : ((1ull << c->aqd) - 1u);
    if ((c->aseq & SEQ_MASK) == 0u) c->aseq++;          /* 通し番号 0 は使わない(as[] の空の印)*/
    c->abase = c->aseq & SEQ_MASK;
    c->ainflight = 0;
    c->aerr = 0;
}

/*=================================================================
 * 位置 pc の WQE の完了を、その枠に入れる(poll_one() から)。read が成功なら、応答を手元の dst へ写す
 * (32 バイトまでは CQE に入っている = scat、それより長ければ枠の中継バッファ)。枠を空きに戻す。
 * ===============================================================*/
void bfm_async_done(struct bfm_conn *c, uint32_t pc, const volatile uint8_t *scat, int rc)
{
    const uint32_t k1 = c->wqe_slot[pc & (SQ_N - 1u)];
    if (k1 == 0u) return;
    c->wqe_slot[pc & (SQ_N - 1u)] = 0u;
    const uint32_t k = k1 - 1u;
    struct bfm_aslot *s = &c->as[k];
    if (!s->busy) return;
    if (rc == BFM_OK && s->kind == BFM_AK_READ) {
        uint8_t *d = (uint8_t *)s->dst;
        if (scat != NULL && s->len <= SCAT_MAX) {
            for (uint32_t i = 0; i < s->len; i++) d[i] = scat[i];
        } else {
            memcpy(d, (const void *)(uintptr_t)(c->abuf + (uint64_t)k * c->abuf_slot), s->len);
        }
    }
    s->rc = rc;
    s->busy = 0;
    c->afree |= 1ull << k;
    c->ainflight--;
    if (rc != BFM_OK) {
        if (c->aerr == 0) c->aerr = rc;
        BFM_LOG("bfm: 接続 %d: 非同期の %s(tag 0x%08x、+0x%llx、%u バイト)がエラーで終わった: %s\n", c->index,
                s->kind == BFM_AK_READ ? "read" : s->kind == BFM_AK_WRITE ? "write" : "write(ACK を確かめない)",
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
            s->kind == BFM_AK_READ ? "read" : s->kind == BFM_AK_WRITE ? "write" : "write(ACK を確かめない)",
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
    struct bfm_aslot *s = &c->as[k];
    s->seq = c->aseq++ & SEQ_MASK;
    if (s->seq == 0u) s->seq = c->aseq++ & SEQ_MASK;   /* 26 ビットで回ったとき、0 は飛ばす */
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
    /* 張り直しより前に出した操作(通し番号が abase より前。26 ビットで回るので、差の半分より後ろを前とみなす):
     * その結果はもう分からない */
    if (((seq - c->abase) & SEQ_MASK) > SEQ_MASK / 2u) return BFM_E_STATE;
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
