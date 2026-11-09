/* bfm_init() / bfm_shutdown() / bfm_poll()。
 *
 * 立ち上げと停止は状態機械で、待つところでは BFM_INPROGRESS を返す。呼び出し側は
 * BFM_INPROGRESS でなくなるまで同じ関数を呼び続ける。
 *   立ち上げ: I_IDLE -> I_PCI -> I_MAP -> I_HCA -> I_DIF -> I_CM -> I_READY
 *   停止:     I_STOP(S_CM -> S_CMD -> S_TEARDOWN -> S_BUS)-> I_IDLE
 * 立ち上げの途中で失敗したら、停止の手順で後始末をしてから失敗を返す。 */
#include "bfm_internal.h"

#include <string.h>

/* 全体の状態。 */
enum {
    I_IDLE = 0,       /* 止まっている(bfm_init() を呼べる)*/
    I_PCI,            /* PCIe の設定(FLR の待ちを含む)*/
    I_MAP,            /* BAR0 を張る */
    I_HCA,            /* HCA の立ち上げ */
    I_DIF,            /* T10-DIF の準備 */
    I_CM,             /* GSI と接続の枠の用意 */
    I_READY,          /* 使える */
    I_STOP            /* 止めている(bfm_shutdown()、または立ち上げの失敗の後始末)*/
};

/* 停止の手順(I_STOP の中)。 */
enum {
    S_CM = 0,         /* 接続を畳み、GSI の送信を待ち、WC の窓を返す */
    S_CMD,            /* 出したままのコマンドの完了を待って捨てる */
    S_TEARDOWN,       /* TEARDOWN_HCA */
    S_BUS,            /* Bus Master を落とし、BAR0 を外し、ポート層を閉じる */
    S_DONE            /* 済んだ */
};

static int s_phase;                         /* 全体の状態(I_*)*/
static int s_stop;                          /* 停止の手順(S_*)*/
static int s_stop_rc;                       /* 止め終えたときに bfm_init() が返す値(立ち上げの失敗の理由)*/
static int s_port_ok;                       /* ポート層を開いた(止めるときに閉じる)*/

bfm_lock_t g_bfm_ctl_lock;                  /* 接続をまたぐ状態(GSI・接続の枠・FW のコマンド・EQ)を守る鍵 */

static int check_config(void)
{
    const uint64_t m = BFM_PAGE - 1u;
    if ((g_bfm_dma_base & m) || (g_bfm_dma_size & m) || (g_bfm_mr_base & m) || (g_bfm_mr_size & m) ||
        g_bfm_dma_size == 0 || g_bfm_mr_size == 0) {
        BFM_LOG("bfm: g_bfm_dma_* / g_bfm_mr_* は 4KiB の境界・倍数にすること\n");
        return BFM_E_PARAM;
    }
    if (g_bfm_dma_base < g_bfm_mr_base + g_bfm_mr_size && g_bfm_mr_base < g_bfm_dma_base + g_bfm_dma_size) {
        BFM_LOG("bfm: DMA 領域と公開メモリが重なっている\n");
        return BFM_E_PARAM;
    }
    if (g_bfm_local_ip == 0) {
        BFM_LOG("bfm: g_bfm_local_ip が 0\n");
        return BFM_E_PARAM;
    }
    if (g_bfm_stage_size < BFM_PAGE) g_bfm_stage_size = BFM_PAGE;
    if (g_bfm_dif_buf_size < 2u * BFM_PAGE) g_bfm_dif_buf_size = 2u * BFM_PAGE;
    if (g_bfm_signal_every == 0) g_bfm_signal_every = 1;
    if (g_bfm_max_conn == 0 || g_bfm_max_conn > BFM_MAX_CONN) g_bfm_max_conn = BFM_MAX_CONN;
    return BFM_OK;
}

/* 止める手順を始める。rc は止め終えたときに bfm_init() が返す値。 */
static void stop_begin(int rc)
{
    s_phase = I_STOP;
    s_stop = S_CM;
    s_stop_rc = rc;
}

/*=================================================================
 * 止める手順を、待つところまで進める(接続を畳む -> 出したままのコマンドを捨てる ->
 * TEARDOWN_HCA -> Bus Master を落として BAR0 を外し、ポート層を閉じる)。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。
 * ===============================================================*/
static int stop_step(void)
{
    for (;;) {
        switch (s_stop) {
        case S_CM:
            if (bfm_cm_fini_step() == BFM_INPROGRESS) return BFM_INPROGRESS;
            s_stop = S_CMD;
            break;
        case S_CMD:
            if (g_bfm.cmdq != NULL && mlx5_cmd_drain(&g_bfm) == BFM_INPROGRESS) return BFM_INPROGRESS;
            s_stop = S_TEARDOWN;
            break;
        case S_TEARDOWN:
            if (mlx5_hca_teardown(&g_bfm) == BFM_INPROGRESS) return BFM_INPROGRESS;
            s_stop = S_BUS;
            break;
        case S_BUS:
            if (g_bfm.bar0) {
                bfm_pci_bus_master(0);
                bfm_port_bar0_unmap();
                g_bfm.bar0 = NULL;
            }
            if (s_port_ok) bfm_port_fini();
            s_port_ok = 0;
            s_stop = S_DONE;
            break;
        default:   /* S_DONE */
            return BFM_OK;
        }
    }
}

int bfm_init(void)
{
    int rc = BFM_OK;
    if (s_phase == I_READY) return BFM_OK;
    if (s_phase == I_STOP) {
        if (stop_step() == BFM_INPROGRESS) return BFM_INPROGRESS;
        s_phase = I_IDLE;
        return s_stop_rc;
    }
    if (s_phase == I_IDLE) {
        if ((rc = check_config()) != BFM_OK) return rc;
        memset(&g_bfm, 0, sizeof(g_bfm));
        memset(g_bfm_conn, 0, sizeof(g_bfm_conn));
        g_bfm.nconn = g_bfm_max_conn;
        bfm_unlock(&g_bfm_ctl_lock);
        if (bfm_port_init() != 0) {
            BFM_LOG("bfm: ポート層の初期化に失敗\n");
            return BFM_E_HW;
        }
        s_port_ok = 1;
        bfm_mem_reset();
        bfm_pci_setup_reset();
        mlx5_hca_init_reset();
        bfm_dif_init_reset();
        bfm_cm_reset();
        s_phase = I_PCI;
    }
    for (;;) {
        switch (s_phase) {
        case I_PCI:
            if ((rc = bfm_pci_setup_step(&g_bfm)) == BFM_OK) s_phase = I_MAP;
            break;
        case I_MAP:
            g_bfm.bar0 = bfm_port_bar0_map(g_bfm.bar0_phys, g_bfm.bar0_size);
            if (g_bfm.bar0 == NULL) {
                BFM_LOG("bfm: BAR0 を張れない\n");
                rc = BFM_E_HW;
            } else {
                s_phase = I_HCA;
            }
            break;
        case I_HCA:
            if ((rc = mlx5_hca_init_step(&g_bfm)) == BFM_OK) s_phase = I_DIF;
            break;
        case I_DIF:
            if ((rc = bfm_dif_init_step()) == BFM_OK) s_phase = I_CM;
            break;
        case I_CM:
            if ((rc = bfm_cm_init_step()) != BFM_OK) break;
            BFM_LOG("bfm: 準備完了(DMA 領域 %llu KiB 使用 / %llu KiB、接続の枠 %u、inline %u バイトまで、"
                    "BlueFlame の窓 %u バイト)\n", (unsigned long long)(bfm_mem_used() >> 10),
                    (unsigned long long)(g_bfm_dma_size >> 10), g_bfm.nconn, bfm_inline_max(),
                    g_bfm.bf_buf_size);
            s_phase = I_READY;
            return BFM_OK;
        default:
            return BFM_E_STATE;
        }
        if (rc == BFM_INPROGRESS) return BFM_INPROGRESS;
        if (rc != BFM_OK) {
            /* 失敗: 取ったものを返してから失敗を返す(後始末が待つなら BFM_INPROGRESS)*/
            stop_begin(rc);
            if (stop_step() == BFM_INPROGRESS) return BFM_INPROGRESS;
            s_phase = I_IDLE;
            return rc;
        }
    }
}

int bfm_shutdown(void)
{
    if (s_phase == I_IDLE) return BFM_OK;
    if (s_phase != I_STOP) stop_begin(BFM_E_STATE);
    if (stop_step() == BFM_INPROGRESS) return BFM_INPROGRESS;
    s_phase = I_IDLE;
    BFM_LOG("bfm: 止めた\n");
    return BFM_OK;
}

int bfm_poll(void)
{
    if (s_phase != I_READY) return 0;
    bfm_lock(&g_bfm_ctl_lock);                 /* FW のコマンド(ページ要求への答え)と GSI を 1 つずつに */
    const int n = mlx5_eq_poll(&g_bfm) + bfm_cm_poll();
    bfm_unlock(&g_bfm_ctl_lock);
    return n;
}
