/* 代表 API bfm_run()、ポートの立ち上げの状態機械 bfm_dev_step()、bfm_shutdown()。
 *
 * bfm_run() は、どのコアから何本同時に呼んでもよい。呼ぶたびに次を待たずに進める。
 *   1. 最初の 1 回(鍵 g_bfm_ctl_lock を取れたコア): 設定を確かめ、ポート層を開き(DMA 領域・公開メモリ)、
 *      接続の表を作る(bfm_cm_table_init())。
 *   2. 接続の表を回り、行の鍵(c->lock)と g_bfm_ctl_lock の両方を取れた行について、その行の状態機械を
 *      1 つ進める(bfm_cm_conn_step())。取れなければ待たずに次の行へ行く。回り始める行はコアごとにずらす。
 *   3. 立ち上がったポートの EQ(FW のページ要求・エラー)と GSI(相手からの BYE など)を処理する。
 *   4. 表の全部の行の状態をまとめて返す。
 * ポートの立ち上げは、そのポートの行が K_PORT の間に bfm_dev_step() で進める(最初に鍵を取ったコアが
 * 最初の手順を処理し、次の手順も、そのとき鍵を取ったコアが処理する)。
 *   ポートの立ち上げ: D_IDLE -> D_PCI -> D_MAP -> D_HCA -> D_DIF -> D_CM -> D_READY
 *   失敗:            D_STOP(S_CM -> S_CMD -> S_TEARDOWN -> S_BUS)-> D_FAILED
 *   停止:            D_STOP -> D_IDLE(bfm_shutdown())
 * 待つところでは BFM_INPROGRESS を返し、回って待たない(bfm_shutdown() の鍵だけは回って取る)。 */
#include "bfm_internal.h"

#include <string.h>

/* ポートの立ち上げの状態(bfm_dev_t の phase)。 */
enum {
    D_IDLE = 0,       /* 止まっている(次の bfm_dev_step() で立ち上げを始める)*/
    D_PCI,            /* PCIe の設定(FLR の待ちを含む)*/
    D_MAP,            /* BAR0 を張る */
    D_HCA,            /* HCA の立ち上げ(公開メモリの MKey もここで作る)*/
    D_DIF,            /* T10-DIF の準備 */
    D_CM,             /* GSI と接続の枠の用意 */
    D_READY,          /* 使える */
    D_STOP,           /* 止めている(bfm_shutdown()、または立ち上げの失敗の後始末)*/
    D_FAILED          /* 立ち上げに失敗して止めた(bfm_shutdown() の後にやり直す)*/
};

/* 止める手順(D_STOP の中。bfm_dev_t の stop)。 */
enum {
    S_CM = 0,         /* 接続を畳み、GSI の送信を待ち、WC の窓を返す */
    S_CMD,            /* 出したままのコマンドの完了を待って捨てる */
    S_TEARDOWN,       /* TEARDOWN_HCA */
    S_BUS,            /* Bus Master を落とし、BAR0 を外す */
    S_DONE            /* 済んだ */
};

/* 全体の状態(s_run)。 */
enum {
    R_IDLE = 0,       /* 止まっている(次の bfm_run() で最初から立ち上げる)*/
    R_RUN,            /* 動いている(接続の表がある)*/
    R_STOP            /* bfm_shutdown() の途中 */
};

static int s_run;                           /* 全体の状態(R_*。acquire / release で読み書きする)*/
static uint32_t s_rr;                       /* 表を回り始める行をコアごとにずらすための通し番号 */

bfm_lock_t g_bfm_ctl_lock;                  /* 接続をまたぐ状態(GSI・接続の枠・FW のコマンド・EQ)を守る鍵 */

static int check_config(void)
{
    const uint64_t m = BFM_PAGE - 1u;
    int used = 0;
    if ((g_bfm_dma_base & m) || (g_bfm_dma_size & m) || (g_bfm_mr_base & m) || (g_bfm_mr_size & m) ||
        g_bfm_dma_size == 0 || g_bfm_mr_size == 0) {
        BFM_LOG("bfm: g_bfm_dma_* / g_bfm_mr_* は 4KiB の境界・倍数にすること\n");
        return BFM_E_PARAM;
    }
    if (g_bfm_dma_base < g_bfm_mr_base + g_bfm_mr_size && g_bfm_mr_base < g_bfm_dma_base + g_bfm_dma_size) {
        BFM_LOG("bfm: DMA 領域と公開メモリが重なっている\n");
        return BFM_E_PARAM;
    }
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        if (g_bfm_port_conn[p] == 0) continue;
        used = 1;
        if (g_bfm_port_conn[p] > BFM_MAX_PORT_CONN) g_bfm_port_conn[p] = BFM_MAX_PORT_CONN;
        if (g_bfm_local_ip[p] == 0 || g_bfm_peer_ip[p] == 0 || g_bfm_local_ip[p] == g_bfm_peer_ip[p]) {
            BFM_LOG("bfm: ポート %u: g_bfm_local_ip[] と g_bfm_peer_ip[] は 0 でない別の値にすること"
                    "(小さいほうのノードが掛け側になる)\n", p);
            return BFM_E_PARAM;
        }
    }
    if (!used) {
        BFM_LOG("bfm: 使うポートが無い(g_bfm_port_conn[] が全部 0)\n");
        return BFM_E_PARAM;
    }
    if (g_bfm_stage_size < BFM_PAGE) g_bfm_stage_size = BFM_PAGE;
    if (g_bfm_dif_buf_size < 2u * BFM_PAGE) g_bfm_dif_buf_size = 2u * BFM_PAGE;
    if (g_bfm_signal_every == 0) g_bfm_signal_every = 1;
    if (g_bfm_req_retry_ms == 0) g_bfm_req_retry_ms = 1;
    return BFM_OK;
}

/*=================================================================
 * 最初の bfm_run() で 1 回だけ: 設定を確かめ、装置(ポート)と接続の表を作り、ポート層を開く
 * (DMA 領域と公開メモリを張る)。g_bfm_ctl_lock の中で呼ぶ。
 * ===============================================================*/
static int run_start(void)
{
    int rc;
    if ((rc = check_config()) != BFM_OK) return rc;
    memset(g_bfm_dev, 0, sizeof(g_bfm_dev));
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        bfm_dev_t *d = &g_bfm_dev[p];
        d->port = p;
        d->used = (g_bfm_port_conn[p] != 0) ? 1u : 0u;
        d->nconn = d->used ? g_bfm_port_conn[p] : 0u;
        d->initiator = (g_bfm_local_ip[p] < g_bfm_peer_ip[p]) ? 1u : 0u;
        d->phase = D_IDLE;
    }
    bfm_cm_table_init();
    if (bfm_port_init() != 0) {
        BFM_LOG("bfm: ポート層の初期化に失敗\n");
        return BFM_E_HW;
    }
    bfm_mem_reset();
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        const bfm_dev_t *d = &g_bfm_dev[p];
        if (!d->used) continue;
        BFM_LOG("bfm: ポート %u: 接続 %u 本、%s(自分 %u.%u.%u.%u、相手 %u.%u.%u.%u)\n", p, d->nconn,
                d->initiator ? "掛け側" : "受け側",
                g_bfm_local_ip[p] >> 24, (g_bfm_local_ip[p] >> 16) & 0xffu, (g_bfm_local_ip[p] >> 8) & 0xffu,
                g_bfm_local_ip[p] & 0xffu, g_bfm_peer_ip[p] >> 24, (g_bfm_peer_ip[p] >> 16) & 0xffu,
                (g_bfm_peer_ip[p] >> 8) & 0xffu, g_bfm_peer_ip[p] & 0xffu);
    }
    return BFM_OK;
}

/* ポート d を止める手順を始める。rc は止め終えたときの結果(立ち上げの失敗の理由。停止なら BFM_OK)。 */
static void dev_stop_begin(bfm_dev_t *d, int rc)
{
    d->phase = D_STOP;
    d->stop = S_CM;
    d->stop_rc = rc;
}

/*=================================================================
 * ポート d を止める手順を、待つところまで進める(接続を畳む -> 出したままのコマンドを捨てる ->
 * TEARDOWN_HCA -> Bus Master を落として BAR0 を外す)。g_bfm_ctl_lock の中で呼ぶ。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。
 * ===============================================================*/
static int dev_stop_step(bfm_dev_t *d)
{
    for (;;) {
        switch (d->stop) {
        case S_CM:
            if (bfm_cm_fini_step(d) == BFM_INPROGRESS) return BFM_INPROGRESS;
            d->stop = S_CMD;
            break;
        case S_CMD:
            if (d->cmdq != NULL && mlx5_cmd_drain(d) == BFM_INPROGRESS) return BFM_INPROGRESS;
            d->stop = S_TEARDOWN;
            break;
        case S_TEARDOWN:
            if (mlx5_hca_teardown(d) == BFM_INPROGRESS) return BFM_INPROGRESS;
            d->stop = S_BUS;
            break;
        case S_BUS:
            if (d->bar0_size != 0) bfm_pci_bus_master(d, 0);   /* PCIe の設定が済んでいれば(BAR0 を張れなくても)*/
            if (d->bar0) {
                bfm_port_bar0_unmap(d->port);
                d->bar0 = NULL;
            }
            d->stop = S_DONE;
            break;
        default:   /* S_DONE */
            return BFM_OK;
        }
    }
}

/*=================================================================
 * ポート d の立ち上げを、待つところまで進める(接続の状態機械の K_PORT から。g_bfm_ctl_lock の中で)。
 * PCIe の設定 -> BAR0 を張る -> HCA(公開メモリの MKey を含む)-> T10-DIF -> GSI と接続の枠。
 * 途中で失敗したら、取ったものを返す手順(D_STOP)を同じように進めてから D_FAILED にする。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 使える。負 = 失敗した(その理由)。
 * ===============================================================*/
int bfm_dev_step(bfm_dev_t *d)
{
    int rc = BFM_OK;
    switch (d->phase) {
    case D_READY:
        return BFM_OK;
    case D_FAILED:
        return d->fail_rc;
    case D_STOP:
        if (dev_stop_step(d) == BFM_INPROGRESS) return BFM_INPROGRESS;
        d->phase = D_FAILED;
        d->fail_rc = d->stop_rc;
        return d->fail_rc;
    case D_IDLE:
        bfm_pci_setup_reset(d);
        mlx5_hca_init_reset(d);
        bfm_dif_init_reset(d);
        bfm_cm_reset(d);
        BFM_DBG("bfm: ポート %u の立ち上げを始める\n", d->port);
        d->phase = D_PCI;
        break;
    default:
        break;
    }
    for (;;) {
        switch (d->phase) {
        case D_PCI:
            if ((rc = bfm_pci_setup_step(d)) == BFM_OK) d->phase = D_MAP;
            break;
        case D_MAP:
            d->bar0 = bfm_port_bar0_map(d->port, d->bar0_phys, d->bar0_size);
            if (d->bar0 == NULL) {
                BFM_LOG("bfm: ポート %u: BAR0 を張れない\n", d->port);
                rc = BFM_E_HW;
            } else {
                d->phase = D_HCA;
            }
            break;
        case D_HCA:
            if ((rc = mlx5_hca_init_step(d)) == BFM_OK) d->phase = D_DIF;
            break;
        case D_DIF:
            if ((rc = bfm_dif_init_step(d)) == BFM_OK) d->phase = D_CM;
            break;
        case D_CM:
            if ((rc = bfm_cm_init_step(d)) != BFM_OK) break;
            BFM_LOG("bfm: ポート %u: 準備完了(DMA 領域 %llu KiB 使用 / %llu KiB、接続 %u 本、inline %u バイトまで、"
                    "BlueFlame の窓 %u バイト)\n", d->port, (unsigned long long)(bfm_mem_used() >> 10),
                    (unsigned long long)(g_bfm_dma_size >> 10), d->nconn, bfm_inline_max(), d->bf_buf_size);
            d->phase = D_READY;
            return BFM_OK;
        default:
            return BFM_E_STATE;
        }
        if (rc == BFM_INPROGRESS) return BFM_INPROGRESS;
        if (rc != BFM_OK) {
            /* 失敗: 取ったものを返してから失敗にする(後始末が待つなら BFM_INPROGRESS)*/
            BFM_LOG("bfm: ポート %u の立ち上げに失敗(%s)。後始末をする\n", d->port, bfm_strerror(rc));
            dev_stop_begin(d, rc);
            if (dev_stop_step(d) == BFM_INPROGRESS) return BFM_INPROGRESS;
            d->phase = D_FAILED;
            d->fail_rc = rc;
            return rc;
        }
    }
}

int bfm_run(void)
{
    int st = __atomic_load_n(&s_run, __ATOMIC_ACQUIRE);
    if (st == R_STOP) return BFM_E_STATE;
    if (st == R_IDLE) {
        /* 最初の 1 回: 鍵を取れたコアが表を作る(取れなければ、ほかのコアが作っている)*/
        if (!bfm_trylock(&g_bfm_ctl_lock)) return BFM_INPROGRESS;
        int rc = BFM_OK;
        if (__atomic_load_n(&s_run, __ATOMIC_ACQUIRE) == R_IDLE) {
            rc = run_start();
            if (rc == BFM_OK) __atomic_store_n(&s_run, R_RUN, __ATOMIC_RELEASE);
        }
        bfm_unlock(&g_bfm_ctl_lock);
        if (rc != BFM_OK) return rc;
        if (__atomic_load_n(&s_run, __ATOMIC_ACQUIRE) != R_RUN) return BFM_E_STATE;
    }

    /* 接続の表を回り、鍵を取れた行の状態を 1 つ進める */
    const unsigned start = __atomic_fetch_add(&s_rr, 1u, __ATOMIC_RELAXED) % BFM_MAX_CONN;
    for (unsigned k = 0; k < BFM_MAX_CONN; k++) {
        struct bfm_conn *c = &g_bfm_conn[(start + k) % BFM_MAX_CONN];
        if (!c->used || !bfm_trylock(&c->lock)) continue;
        if (bfm_trylock(&g_bfm_ctl_lock)) {
            if (__atomic_load_n(&s_run, __ATOMIC_ACQUIRE) == R_RUN) (void)bfm_cm_conn_step(c);
            bfm_unlock(&g_bfm_ctl_lock);
        }
        bfm_unlock(&c->lock);
    }

    /* 立ち上がったポートの EQ(FW のページ要求・エラー)と GSI(相手からの BYE など)*/
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        bfm_dev_t *d = &g_bfm_dev[p];
        if (!d->used || d->phase != D_READY || !bfm_trylock(&g_bfm_ctl_lock)) continue;
        if (d->phase == D_READY && __atomic_load_n(&s_run, __ATOMIC_ACQUIRE) == R_RUN) {
            (void)mlx5_eq_poll(d);
            (void)bfm_cm_poll(d);
        }
        bfm_unlock(&g_bfm_ctl_lock);
    }

    /* 全体の様子: 失敗 > エラー・切断のまま > 途中 > 全部使える */
    int fail = BFM_OK, down = 0, all_up = 1;
    for (unsigned i = 0; i < BFM_MAX_CONN; i++) {
        const struct bfm_conn *c = &g_bfm_conn[i];
        if (!c->used) continue;
        const int s = bfm_cm_conn_state(c);
        if (s == BFM_ST_FAILED && fail == BFM_OK) fail = (c->result < 0) ? c->result : BFM_E_HW;
        if (s == BFM_ST_DOWN) down = 1;
        if (s != BFM_ST_UP) all_up = 0;
    }
    if (fail != BFM_OK) return fail;
    if (down) return BFM_E_STATE;
    return all_up ? BFM_OK : BFM_INPROGRESS;
}

int bfm_shutdown(void)
{
    if (__atomic_load_n(&s_run, __ATOMIC_ACQUIRE) == R_IDLE) return BFM_OK;
    __atomic_store_n(&s_run, R_STOP, __ATOMIC_RELEASE);   /* bfm_run() は表を進めなくなる */
    int busy = 0;
    bfm_lock(&g_bfm_ctl_lock);
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        bfm_dev_t *d = &g_bfm_dev[p];
        if (!d->used || d->phase == D_IDLE || d->phase == D_FAILED) continue;   /* D_FAILED は止め終えている */
        if (d->phase != D_STOP) dev_stop_begin(d, BFM_OK);
        if (dev_stop_step(d) == BFM_INPROGRESS) busy = 1;
        else d->phase = D_IDLE;
    }
    bfm_unlock(&g_bfm_ctl_lock);
    if (busy) return BFM_INPROGRESS;
    bfm_port_fini();
    __atomic_store_n(&s_run, R_IDLE, __ATOMIC_RELEASE);
    BFM_LOG("bfm: 止めた\n");
    return BFM_OK;
}
