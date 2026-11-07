/* データ経路: 相手のメモリへの write / read / atomic を BlueFlame で出す。
 *
 * 1 個の操作 = 1 個の WQE。WQE はまず手元の作業域で組み、SQ のリング(ホストメモリ)へ
 * 写し、doorbell record を進めてから、**同じ WQE を UAR の BlueFlame の窓へ MMIO で
 * 書く**。窓は write-combining なので 64 バイトが 1 個の PCIe の書き込みにまとまり、
 * NIC はドアベルと一緒に WQE を受け取る(リングへ WQE を DMA で読みに来ない)。
 * 窓へ書いたものが 1 回にまとまらなかったときは、NIC はそれを捨ててリングから読む
 * ので、結果は同じ(遅くなるだけ)。窓は 2 面あり、交互に使う(rdma-core の mlx5 と同じ)。
 *
 * 書き込みの小さいものは、データを WQE に埋め込む(inline)。データを読みに来る
 * DMA も無くなるので、write は「MMIO の書き込み 1 回」で相手へ出ていく。
 *
 * 読み出しと atomic は必ず完了通知(CQE)を求める。scatter to CQE(QP の cs_req)を
 * 有効にした QP に通知なしの read を出すと QP がエラーになる(vfio_nvme で確認)。
 * 32 バイトまでの応答は CQE に入るので、CPU がそこから写す。 */
#include "bfm_internal.h"

#include <string.h>

#define SQ_N      (1u << BFM_RC_SQ_LOG)
#define CQ_N      (1u << BFM_RC_CQ_LOG)
#define INLINE_MAX (BFM_WQE_MAX - 32u - 4u)   /* ctrl 16 + raddr 16 + inline の頭 4 */
#define SCAT_MAX  32u

static inline int conn_usable(const struct bfm_conn *c)
{
    return c != NULL && c->state == BFM_CS_UP;
}

static int map_syndrome(uint8_t synd)
{
    switch (synd) {
    case 0x12: case 0x13: case 0x14: return BFM_E_REMOTE;
    case 0x15: case 0x16: return BFM_E_RETRY;
    default: return BFM_E_CQE;
    }
}

static const char *syndrome_str(uint8_t synd)
{
    switch (synd) {
    case 0x01: return "LOCAL_LENGTH";
    case 0x02: return "LOCAL_QP_OP";
    case 0x04: return "LOCAL_PROT";
    case 0x05: return "WR_FLUSH(先のエラーで QP が止まっている)";
    case 0x10: return "BAD_RESP";
    case 0x11: return "LOCAL_ACCESS";
    case 0x12: return "REMOTE_INVAL_REQ(同時 read/atomic の超過・相手が受けない操作)";
    case 0x13: return "REMOTE_ACCESS(rkey・範囲・権限)";
    case 0x14: return "REMOTE_OP";
    case 0x15: return "TRANSPORT_RETRY_EXC(相手に届かない)";
    case 0x16: return "RNR_RETRY_EXC";
    default:   return "?";
    }
}

/*=================================================================
 * CQE を 1 個読む。1 = 正常な完了、0 = まだ無い、負 = エラー完了(接続をエラーにする)。
 * 正常なら、その WQE までを済んだ扱いにする(CQE は投稿の順に来る)。
 * ===============================================================*/
static int poll_one(struct bfm_conn *c)
{
    volatile uint8_t *cqe = c->qp.cq + (c->qp.cq_cc & (CQ_N - 1u)) * 64u;
    const uint8_t op_own = cqe[63];
    if ((op_own & 1u) != ((c->qp.cq_cc >> BFM_RC_CQ_LOG) & 1u) || (op_own >> 4) == MLX5_CQE_INVALID) return 0;
    bfm_rmb();
    const uint8_t opcode = (uint8_t)(op_own >> 4);
    const uint16_t cnt = bfm_rd16be(cqe + 60);
    c->qp.cq_cc++;
    c->qp.cq_dbr[0] = bfm_cpu_to_be32(c->qp.cq_cc & 0xffffffu);
    if (opcode == MLX5_CQE_REQ_ERR || opcode == MLX5_CQE_RESP_ERR || opcode == MLX5_CQE_SIG_ERR) {
        c->last_synd = cqe[55];
        c->last_vsynd = cqe[54];
        if (c->state == BFM_CS_UP) {
            BFM_LOG("bfm: 接続 %d: エラー完了 syndrome 0x%02x(vendor 0x%02x)%s。接続は使えなくなった\n",
                    c->index, c->last_synd, c->last_vsynd, syndrome_str(c->last_synd));
            c->state = BFM_CS_ERROR;
        }
        return map_syndrome(c->last_synd);
    }
    const uint32_t pc = c->sq_done + ((uint32_t)(uint16_t)(cnt - (uint16_t)c->sq_done));
    c->sq_done = pc + c->wqe_bbs[pc & (SQ_N - 1u)];
    c->last_scat = (op_own & 0x04u) ? cqe : NULL;
    return 1;
}

/* target(WQEBB の位置)まで済むのを待つ。 */
static int wait_done(struct bfm_conn *c, uint32_t target)
{
    const uint64_t t0 = bfm_port_time_ns();
    uint32_t spins = 0;
    while ((int32_t)(c->sq_done - target) < 0) {
        const int r = poll_one(c);
        if (r < 0) return r;
        if (r == 0 && (++spins & 0xfffu) == 0 &&
            bfm_port_time_ns() - t0 > (uint64_t)g_bfm_op_timeout_ms * 1000000u) {
            BFM_LOG("bfm: 接続 %d: %u ms 待っても完了しない\n", c->index, g_bfm_op_timeout_ms);
            c->state = BFM_CS_ERROR;
            return BFM_E_TIMEOUT;
        }
    }
    return BFM_OK;
}

/*=================================================================
 * 組んだ WQE(w、bbs 個の WQEBB)をリングへ写し、ドアベルを鳴らす。
 * 窓に収まれば WQE 本体を BlueFlame で、収まらなければ先頭 8 バイトだけを書く。
 * ===============================================================*/
static int post(struct bfm_conn *c, const uint64_t *w, uint32_t bbs)
{
    while ((c->qp.sq_pc + bbs) - c->sq_done > SQ_N) {
        const int r = poll_one(c);
        if (r < 0) return r;
        if (r == 0) {
            const int w_rc = wait_done(c, c->sq_done + 1u);   /* エラー完了も時間切れもそのまま返す */
            if (w_rc != BFM_OK) return w_rc;
        }
    }
    const uint32_t pc = c->qp.sq_pc;
    for (uint32_t b = 0; b < bbs; b++) {
        volatile uint64_t *dst = (volatile uint64_t *)(volatile void *)(c->qp.sq + ((pc + b) & (SQ_N - 1u)) * MLX5_SEND_WQE_BB);
        for (unsigned k = 0; k < 8u; k++) dst[k] = w[b * 8u + k];
    }
    c->wqe_bbs[pc & (SQ_N - 1u)] = (uint8_t)bbs;
    c->qp.sq_pc = pc + bbs;

    bfm_dma_wmb();
    c->qp.dbr[1] = bfm_cpu_to_be32(c->qp.sq_pc & 0xffffu);
    bfm_dma_wmb();
    if (c->bf != NULL && bbs * MLX5_SEND_WQE_BB <= g_bfm.bf_buf_size) {
        volatile uint64_t *reg = (volatile uint64_t *)(volatile void *)(c->bf + c->bf_off);
        for (uint32_t k = 0; k < bbs * 8u; k++) reg[k] = w[k];
    } else {
        bfm_mmio_wr64_raw(c->db + c->bf_off, w[0]);
    }
    bfm_wc_flush();
    c->bf_off ^= g_bfm.bf_buf_size;
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * WQE を組む(w は 8 バイト単位の作業域。BFM_WQE_MAX バイト)
 * ------------------------------------------------------------------- */

static inline void put32(uint64_t *w, unsigned byte_off, uint32_t v)
{
    uint32_t be = bfm_cpu_to_be32(v);
    memcpy((uint8_t *)w + byte_off, &be, 4);
}
static inline void put64(uint64_t *w, unsigned byte_off, uint64_t v)
{
    uint64_t be = bfm_cpu_to_be64(v);
    memcpy((uint8_t *)w + byte_off, &be, 8);
}

static void build_ctrl(struct bfm_conn *c, uint64_t *w, uint32_t opcode, uint32_t ds, int signaled)
{
    put32(w, 0, ((c->qp.sq_pc & 0xffffu) << 8) | opcode);
    put32(w, 4, c->qpn8 | ds);
    put32(w, 8, signaled ? MLX5_WQE_CTRL_CQ_UPDATE : 0u);    /* fm_ce_se(byte 11)*/
    put32(w, 12, 0);
}

static void build_raddr(struct bfm_conn *c, uint64_t *w, uint64_t off)
{
    put64(w, 16, c->raddr + off);
    put32(w, 24, c->rkey);
    put32(w, 28, 0);
}

static void build_dseg(uint64_t *w, unsigned at, uint32_t len, uint64_t addr)
{
    put32(w, at, len);
    put32(w, at + 4u, g_bfm.lkey);
    put64(w, at + 8u, addr);
}

/* inline の write。WQEBB の数を返す。 */
static uint32_t build_write_inline(struct bfm_conn *c, uint64_t *w, uint64_t off, const void *src,
                                   uint32_t len, int signaled)
{
    const uint32_t ds = 2u + (4u + len + 15u) / 16u;
    memset(w, 0, ((ds * 16u + 63u) / 64u) * 64u);
    build_ctrl(c, w, MLX5_OPCODE_RDMA_WRITE, ds, signaled);
    build_raddr(c, w, off);
    put32(w, 32, MLX5_INLINE_SEG | len);
    memcpy((uint8_t *)w + 36, src, len);
    return (ds + 3u) / 4u;
}

static int check_range(const struct bfm_conn *c, uint64_t off, uint32_t len)
{
    if (!conn_usable(c)) return BFM_E_STATE;
    if (len == 0 || c->rlen == 0 || off > c->rlen || len > c->rlen - off) return BFM_E_PARAM;
    return BFM_OK;
}

/* ---------------------------------------------------------------------
 * 公開する関数
 * ------------------------------------------------------------------- */

int bfm_ops_attach(struct bfm_conn *c)
{
    c->qpn8 = c->qp.qpn << 8;
    c->sq_done = c->qp.sq_pc;
    c->unsig = 0;
    c->bf_off = 0;
    c->last_synd = c->last_vsynd = 0;
    c->last_scat = NULL;
    return BFM_OK;
}

void bfm_ops_detach(struct bfm_conn *c)
{
    c->last_scat = NULL;
}

/* EQ でその QP のエラーを知ったとき。 */
void bfm_conn_mark_error(uint32_t qpn, const char *why)
{
    for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
        struct bfm_conn *c = &g_bfm_conn[i];
        if (c->state == BFM_CS_UP && c->qp.has_qp && c->qp.qpn == qpn) {
            BFM_LOG("bfm: 接続 %u: %s。接続は使えなくなった\n", i, why);
            c->state = BFM_CS_ERROR;
        }
    }
}

/*=================================================================
 * 性能の試験用(bfm_internal.h)。完了を待たずに 1 個出す。完了通知は必ず求めるので、
 * 完了は bfm_ops_poll1() で 1 個ずつ、出した順に拾える。
 *   is_read = 0: write。len <= INLINE_MAX なら src を WQE に埋め込み、そうでなければ
 *                中継バッファの stage_off から送る(中身は呼び出し側が置く)。
 *   is_read = 1: read。中継バッファの stage_off へ読む。
 * ===============================================================*/
int bfm_ops_post_rw(struct bfm_conn *c, int is_read, uint64_t off, uint32_t stage_off,
                    const void *src, uint32_t len)
{
    uint64_t w[BFM_WQE_MAX / 8u];
    int rc = check_range(c, off, len);
    if (rc != BFM_OK) return rc;
    /* 埋め込みの write は中継バッファを使わないので、stage_off は見ない */
    if (!is_read && len <= INLINE_MAX) return post(c, w, build_write_inline(c, w, off, src, len, 1));
    if ((uint64_t)stage_off + len > c->stage_size) return BFM_E_PARAM;
    memset(w, 0, 64);
    build_ctrl(c, w, is_read ? MLX5_OPCODE_RDMA_READ : MLX5_OPCODE_RDMA_WRITE, 3u, 1);
    build_raddr(c, w, off);
    build_dseg(w, 32, len, bfm_dma_addr(c->stage + stage_off));
    return post(c, w, 1u);
}

/* 完了を 1 個拾う。1 = 拾った、0 = まだ無い、負 = エラー完了。 */
int bfm_ops_poll1(struct bfm_conn *c)
{
    return poll_one(c);
}

int bfm_write(bfm_conn_t *c, uint64_t off, const void *src, uint32_t len)
{
    uint64_t w[BFM_WQE_MAX / 8u];
    int rc = check_range(c, off, len);
    if (rc != BFM_OK) return rc;
    if (len <= INLINE_MAX) {
        const uint32_t bbs = build_write_inline(c, w, off, src, len, 1);
        if ((rc = post(c, w, bbs)) != BFM_OK) return rc;
        return wait_done(c, c->qp.sq_pc);
    }
    const uint8_t *s = (const uint8_t *)src;
    while (len > 0) {
        const uint32_t n = (len > c->stage_size) ? c->stage_size : len;
        memcpy((void *)(uintptr_t)c->stage, s, n);
        memset(w, 0, 64);
        build_ctrl(c, w, MLX5_OPCODE_RDMA_WRITE, 3u, 1);
        build_raddr(c, w, off);
        build_dseg(w, 32, n, bfm_dma_addr(c->stage));
        if ((rc = post(c, w, 1u)) != BFM_OK) return rc;
        if ((rc = wait_done(c, c->qp.sq_pc)) != BFM_OK) return rc;
        s += n;
        off += n;
        len -= n;
    }
    return BFM_OK;
}

int bfm_write_post(bfm_conn_t *c, uint64_t off, const void *src, uint32_t len)
{
    uint64_t w[BFM_WQE_MAX / 8u];
    int rc = check_range(c, off, len);
    if (rc != BFM_OK) return rc;
    if (len > INLINE_MAX) return BFM_E_PARAM;
    /* 通知なしが続きすぎると、SQ の空きが分からなくなるので、間隔と残りで通知を挟む。 */
    const int sig = (++c->unsig >= g_bfm_signal_every) ||
                    ((c->qp.sq_pc - c->sq_done) > SQ_N / 2u);
    const uint32_t bbs = build_write_inline(c, w, off, src, len, sig);
    if (sig) c->unsig = 0;
    return post(c, w, bbs);
}

int bfm_flush(bfm_conn_t *c)
{
    if (!conn_usable(c)) return BFM_E_STATE;
    if (c->sq_done == c->qp.sq_pc) return BFM_OK;
    if (c->unsig != 0) {
        /* 最後が通知なしなら、通知ありの NOP を足す(完了は投稿の順なので、NOP の完了 =
         * それまでの write が全部相手に受け取られた)。 */
        uint64_t w[8] = {0};
        build_ctrl(c, w, MLX5_OPCODE_NOP, 1u, 1);
        const int rc = post(c, w, 1u);
        if (rc != BFM_OK) return rc;
        c->unsig = 0;
    }
    return wait_done(c, c->qp.sq_pc);
}

int bfm_read(bfm_conn_t *c, uint64_t off, void *dst, uint32_t len)
{
    uint64_t w[8];
    int rc = check_range(c, off, len);
    if (rc != BFM_OK) return rc;
    uint8_t *d = (uint8_t *)dst;
    while (len > 0) {
        const uint32_t n = (len > c->stage_size) ? c->stage_size : len;
        memset(w, 0, sizeof(w));
        build_ctrl(c, w, MLX5_OPCODE_RDMA_READ, 3u, 1);
        build_raddr(c, w, off);
        build_dseg(w, 32, n, bfm_dma_addr(c->stage));
        if ((rc = post(c, w, 1u)) != BFM_OK) return rc;
        if ((rc = wait_done(c, c->qp.sq_pc)) != BFM_OK) return rc;
        if (c->last_scat != NULL && n <= SCAT_MAX) {
            for (uint32_t i = 0; i < n; i++) d[i] = c->last_scat[i];
        } else {
            memcpy(d, (const void *)(uintptr_t)c->stage, n);
        }
        d += n;
        off += n;
        len -= n;
    }
    return BFM_OK;
}

/* atomic の元の値(8 バイト)を数にする。
 * **CQE に入って返った値は、HCA の設定によらずビッグエンディアン**(ConnectX-4 Lx で確認)。
 * 中継バッファへ書かれた値は、HCA をホスト順に設定してあるのでホスト順。 */
static uint64_t atomic_result(struct bfm_conn *c)
{
    if (c->last_scat != NULL) return bfm_rd64be(c->last_scat);
    uint64_t v;
    memcpy(&v, (const void *)(uintptr_t)c->stage, 8);   /* HCA がホスト順で書く */
    return v;
}

/* 4 バイトの atomic の元の値を数にする。
 * **CQE に入っても中継バッファへ書かれても、ビッグエンディアン**(ConnectX-4 Lx で確認)。
 * ホスト順の設定(atomic_req_8B_endianness_mode)は、4 バイトでは相手のメモリの更新にだけ効く。 */
static uint32_t atomic32_result(struct bfm_conn *c)
{
    if (c->last_scat != NULL) return bfm_rd32be(c->last_scat);
    return bfm_rd32be((const volatile void *)(uintptr_t)c->stage);
}

static int atomic_op(struct bfm_conn *c, uint32_t opcode, uint64_t off, uint64_t swap_add,
                     uint64_t cmp, uint64_t *old)
{
    uint64_t w[8];
    int rc = check_range(c, off, 8u);
    if (rc != BFM_OK) return rc;
    if (((c->raddr + off) & 7u) != 0) return BFM_E_PARAM;
    memset(w, 0, sizeof(w));
    build_ctrl(c, w, opcode, 4u, 1);
    build_raddr(c, w, off);
    put64(w, 32, swap_add);
    put64(w, 40, cmp);
    build_dseg(w, 48, 8u, bfm_dma_addr(c->stage));
    if ((rc = post(c, w, 1u)) != BFM_OK) return rc;
    if ((rc = wait_done(c, c->qp.sq_pc)) != BFM_OK) return rc;
    if (old) *old = atomic_result(c);
    return BFM_OK;
}

/*=================================================================
 * 4 バイトの atomic(拡張 atomic = masked atomic。IB の規格には無い mlx5 の拡張)。
 * ctrl の opmod で大きさを指定し、atomic セグメントは 16 バイト:
 *   CAS: swap・compare・swap_mask・compare_mask(各 4 バイト。mask は全ビット)
 *   FAA: add・field_boundary(0 = 32 ビットで 1 つの数)・予約 8 バイト
 * 両端の装置が拡張 atomic の 4 バイトに対応していること(相手の QP の atomic_mode も)。
 * ===============================================================*/
static int atomic32_op(struct bfm_conn *c, uint32_t opcode, uint64_t off, uint32_t swap_add,
                       uint32_t cmp, uint32_t *old)
{
    uint64_t w[8];
    int rc = check_range(c, off, 4u);
    if (rc != BFM_OK) return rc;
    if (((c->raddr + off) & 3u) != 0) return BFM_E_PARAM;
    if ((g_bfm.atomic_ops & 0x0cu) != 0x0cu || (g_bfm.atomic_size_qp & 0x04u) == 0) return BFM_E_PARAM;
    memset(w, 0, sizeof(w));
    build_ctrl(c, w, opcode, 4u, 1);
    put32(w, 0, (MLX5_OPMOD_EXT_ATOMIC(2u) << 24) | ((c->qp.sq_pc & 0xffffu) << 8) | opcode);
    build_raddr(c, w, off);
    put32(w, 32, swap_add);
    if (opcode == MLX5_OPCODE_ATOMIC_MASKED_CS) {
        put32(w, 36, cmp);
        put32(w, 40, 0xffffffffu);
        put32(w, 44, 0xffffffffu);
    }
    build_dseg(w, 48, 4u, bfm_dma_addr(c->stage));
    if ((rc = post(c, w, 1u)) != BFM_OK) return rc;
    if ((rc = wait_done(c, c->qp.sq_pc)) != BFM_OK) return rc;
    if (old) *old = atomic32_result(c);
    return BFM_OK;
}

int bfm_cas(bfm_conn_t *c, uint64_t off, uint64_t expect, uint64_t desired, uint64_t *old)
{
    return atomic_op(c, MLX5_OPCODE_ATOMIC_CS, off, desired, expect, old);
}

int bfm_faa(bfm_conn_t *c, uint64_t off, uint64_t add, uint64_t *old)
{
    return atomic_op(c, MLX5_OPCODE_ATOMIC_FA, off, add, 0, old);
}

int bfm_cas32(bfm_conn_t *c, uint64_t off, uint32_t expect, uint32_t desired, uint32_t *old)
{
    return atomic32_op(c, MLX5_OPCODE_ATOMIC_MASKED_CS, off, desired, expect, old);
}

int bfm_faa32(bfm_conn_t *c, uint64_t off, uint32_t add, uint32_t *old)
{
    return atomic32_op(c, MLX5_OPCODE_ATOMIC_MASKED_FA, off, add, 0, old);
}

uint32_t bfm_inline_max(void)
{
    return INLINE_MAX;
}

int bfm_bf_active(const bfm_conn_t *c)
{
    return c != NULL && c->bf != NULL;
}

int bfm_is_up(const bfm_conn_t *c)
{
    return conn_usable(c);
}

uint64_t bfm_remote_size(const bfm_conn_t *c)
{
    return c ? c->rlen : 0;
}

uint8_t bfm_last_syndrome(const bfm_conn_t *c)
{
    return c ? c->last_synd : 0;
}

const char *bfm_strerror(int rc)
{
    switch (rc) {
    case BFM_OK:        return "成功";
    case BFM_E_PARAM:   return "引数が不正";
    case BFM_E_STATE:   return "接続が使えない";
    case BFM_E_TIMEOUT: return "時間切れ";
    case BFM_E_HW:      return "FW のコマンドが失敗";
    case BFM_E_NOMEM:   return "資源が足りない";
    case BFM_E_REMOTE:  return "相手が拒否(rkey・範囲・権限)";
    case BFM_E_RETRY:   return "再送が尽きた(相手に届かない)";
    case BFM_E_CQE:     return "エラー完了";
    default:            return "?";
    }
}
