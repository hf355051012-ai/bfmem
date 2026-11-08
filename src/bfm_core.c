/* bfm_init() / bfm_shutdown() / bfm_poll()。 */
#include "bfm_internal.h"

#include <string.h>

static int s_inited;                        /* bfm_init() が済み、bfm_shutdown() をまだ呼んでいない */

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

static void cleanup(int port_ok)
{
    if (g_bfm.hca_enabled) mlx5_hca_teardown(&g_bfm);
    if (g_bfm.bar0) {
        bfm_pci_bus_master(0);
        bfm_port_bar0_unmap();
        g_bfm.bar0 = NULL;
    }
    if (port_ok) bfm_port_fini();
}

int bfm_init(void)
{
    if (s_inited) return BFM_OK;
    int rc = check_config();
    if (rc != BFM_OK) return rc;
    memset(&g_bfm, 0, sizeof(g_bfm));
    memset(g_bfm_conn, 0, sizeof(g_bfm_conn));
    g_bfm.nconn = g_bfm_max_conn;
    bfm_unlock(&g_bfm_ctl_lock);
    if (bfm_port_init() != 0) {
        BFM_LOG("bfm: ポート層の初期化に失敗\n");
        return BFM_E_HW;
    }
    bfm_mem_reset();
    if (bfm_pci_setup(&g_bfm) != 0) {
        cleanup(1);
        return BFM_E_HW;
    }
    g_bfm.bar0 = bfm_port_bar0_map(g_bfm.bar0_phys, g_bfm.bar0_size);
    if (g_bfm.bar0 == NULL) {
        BFM_LOG("bfm: BAR0 を張れない\n");
        cleanup(1);
        return BFM_E_HW;
    }
    if ((rc = mlx5_hca_init(&g_bfm)) != BFM_OK || (rc = bfm_dif_init()) != BFM_OK ||
        (rc = bfm_cm_init()) != BFM_OK) {
        cleanup(1);
        return rc;
    }
    BFM_LOG("bfm: 準備完了(DMA 領域 %llu KiB 使用 / %llu KiB、接続の枠 %u、inline %u バイトまで、"
            "BlueFlame の窓 %u バイト)\n", (unsigned long long)(bfm_mem_used() >> 10),
            (unsigned long long)(g_bfm_dma_size >> 10), g_bfm.nconn, bfm_inline_max(),
            g_bfm.bf_buf_size);
    s_inited = 1;
    return BFM_OK;
}

void bfm_shutdown(void)
{
    if (!s_inited) return;
    bfm_cm_fini();
    cleanup(1);
    s_inited = 0;
    BFM_LOG("bfm: 止めた\n");
}

int bfm_poll(void)
{
    if (!s_inited) return 0;
    bfm_lock(&g_bfm_ctl_lock);                 /* FW のコマンド(ページ要求への答え)と GSI を 1 つずつに */
    const int n = mlx5_eq_poll(&g_bfm) + bfm_cm_poll();
    bfm_unlock(&g_bfm_ctl_lock);
    return n;
}
