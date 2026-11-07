/* bfm_config.h の変数の実体と既定値。
 *
 * 既定値はプラットフォームで分ける。Linux(VFIO)の値は OptiPlex の ConnectX-4 Lx を
 * Precision と直結した試験環境のもの。VxWorks の値は置き場所の例で、そのままでは
 * 動かない(BAR0 のアドレス・メモリの位置・IP は機材に合わせて必ず書き換えること)。 */
#include "bfm_config.h"

#if defined(BFM_PORT_LINUX)

uint32_t    g_bfm_pci_bus        = 0;
uint32_t    g_bfm_pci_dev        = 0;
uint32_t    g_bfm_pci_func       = 0;
uint64_t    g_bfm_pcie_ecam_base = 0;
uint64_t    g_bfm_bar0_addr      = 0;              /* カーネルが割り当てた値を使う */
uint64_t    g_bfm_bar0_wc_va     = 0;              /* ポート層が UAR のページを WC に張り替える */
uint32_t    g_bfm_pcie_flr       = 0;              /* vfio-pci が open のときにリセットする */
uint32_t    g_bfm_pcie_mrrs      = 0;
const char *g_bfm_linux_pci_bdf  = "0000:01:00.1";

/* 仮想アドレス = IOVA としてこの位置に張る(4GiB より上。x86 の MSI の窓 0xfee00000 を避ける)。 */
uint64_t g_bfm_dma_base = 0x100000000ull;
uint64_t g_bfm_dma_size = 0x6000000ull;            /* 96MiB(FW のページ 64MiB を含む)*/
uint64_t g_bfm_mr_base  = 0x110000000ull;
uint64_t g_bfm_mr_size  = 0x1000000ull;            /* 16MiB */

uint32_t g_bfm_local_ip     = 0xC0A8650Bu;          /* 192.168.101.11 */
uint8_t  g_bfm_local_mac[6] = {0, 0, 0, 0, 0, 0};
uint32_t g_bfm_peer_ip      = 0xC0A86515u;          /* 192.168.101.21 */
uint8_t  g_bfm_peer_mac[6]  = {0, 0, 0, 0, 0, 0};

#else /* VxWorks */

uint32_t    g_bfm_pci_bus        = 1;
uint32_t    g_bfm_pci_dev        = 0;
uint32_t    g_bfm_pci_func       = 0;
uint64_t    g_bfm_pcie_ecam_base = 0xE0000000ull;  /* 例: 機材の ECAM の位置に合わせる */
uint64_t    g_bfm_bar0_addr      = 0xC0000000ull;  /* 例: BAR0 の大きさの境界に合わせる */
uint64_t    g_bfm_bar0_wc_va     = 0;              /* BSP が WC の別名を張ったらその先頭 */
uint32_t    g_bfm_pcie_flr       = 1;
uint32_t    g_bfm_pcie_mrrs      = 0;
const char *g_bfm_linux_pci_bdf  = 0;

uint64_t g_bfm_dma_base = 0x40000000ull;            /* 例: OS が使わない物理メモリ */
uint64_t g_bfm_dma_size = 0x6000000ull;
uint64_t g_bfm_mr_base  = 0x46000000ull;
uint64_t g_bfm_mr_size  = 0x1000000ull;

uint32_t g_bfm_local_ip     = 0xC0A8650Au;          /* 例: 192.168.101.10 */
uint8_t  g_bfm_local_mac[6] = {0, 0, 0, 0, 0, 0};
uint32_t g_bfm_peer_ip      = 0;
uint8_t  g_bfm_peer_mac[6]  = {0, 0, 0, 0, 0, 0};

#endif

uint32_t g_bfm_fw_pages_max = 16384u;               /* 64MiB */
uint32_t g_bfm_stage_size   = 65536u;
uint32_t g_bfm_dif_buf_size = 131072u;              /* 2 面 x 64KiB */
uint32_t g_bfm_max_conn     = BFM_MAX_CONN;         /* 64 */

uint16_t g_bfm_cm_port      = 18515u;
uint8_t  g_bfm_path_mtu     = 3u;                   /* 1024 */
uint16_t g_bfm_port_mtu     = 0u;
uint32_t g_bfm_link_wait_ms = 10000u;

uint32_t g_bfm_bf_enable          = 1u;
uint32_t g_bfm_scatter_cqe        = 1u;
uint8_t  g_bfm_ack_timeout        = 17u;            /* 約 0.54 秒 */
uint8_t  g_bfm_retry_count        = 7u;
uint32_t g_bfm_op_timeout_ms      = 5000u;
uint32_t g_bfm_signal_every       = 32u;
uint32_t g_bfm_cpu_awake          = 1u;
uint64_t g_bfm_tsc_hz             = 0u;             /* CPUID / MSR から求める */
uint32_t g_bfm_verbose            = 0u;
