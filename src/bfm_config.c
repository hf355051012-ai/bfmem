/* bfm_config.h の変数の実体と既定値。
 *
 * 既定値はプラットフォームで分ける。Linux(VFIO)の値は OptiPlex の ConnectX-4 Lx を
 * Precision と直結した試験環境のもの。VxWorks の値は置き場所の例で、そのままでは
 * 動かない(BAR0 のアドレス・メモリの位置・IP は機材に合わせて必ず書き換えること)。
 * 各変数の詳しい意味は bfm_config.h のコメント。 */
#include "bfm_config.h"

#if defined(BFM_PORT_LINUX)

uint32_t    g_bfm_pci_bus        = 0;              /* PCIe のバス番号(Linux では使わない)*/
uint32_t    g_bfm_pci_dev        = 0;              /* PCIe のデバイス番号(Linux では使わない)*/
uint32_t    g_bfm_pci_func       = 0;              /* PCIe の function 番号(Linux では使わない)*/
uint64_t    g_bfm_pcie_ecam_base = 0;              /* ECAM の先頭(Linux では使わない)*/
uint64_t    g_bfm_bar0_addr      = 0;              /* BAR0 に割り当てるアドレス。カーネルが割り当てた値を使う */
uint64_t    g_bfm_bar0_wc_va     = 0;              /* BAR0 の WC の別名。ポート層が UAR のページを WC に張り替える */
uint32_t    g_bfm_pcie_flr       = 0;              /* FLR をかけるか。vfio-pci が open のときにリセットする */
uint32_t    g_bfm_pcie_mrrs      = 0;              /* Max Read Request Size。0 = 触らない */
const char *g_bfm_linux_pci_bdf  = "0000:01:00.1"; /* 装置の PCI アドレス(OptiPlex の ConnectX-4 Lx の PF1)*/

/* 仮想アドレス = IOVA としてこの位置に張る(4GiB より上。x86 の MSI の窓 0xfee00000 を避ける)。 */
uint64_t g_bfm_dma_base = 0x100000000ull;           /* DMA 領域の先頭 */
uint64_t g_bfm_dma_size = 0x6000000ull;             /* DMA 領域の大きさ。96MiB(FW のページ 64MiB を含む)*/
uint64_t g_bfm_mr_base  = 0x110000000ull;           /* 公開メモリの先頭 */
uint64_t g_bfm_mr_size  = 0x40000000ull;            /* 公開メモリの大きさ。1GiB */

uint32_t g_bfm_local_ip     = 0xC0A8650Bu;          /* 自分の IPv4。192.168.101.11 */
uint8_t  g_bfm_local_mac[6] = {0, 0, 0, 0, 0, 0};   /* 自分の MAC。全部 0 = NIC の固有の MAC */
uint32_t g_bfm_peer_ip      = 0xC0A86515u;          /* 既定の相手の IPv4。192.168.101.21 */
uint8_t  g_bfm_peer_mac[6]  = {0, 0, 0, 0, 0, 0};   /* 既定の相手の MAC。全部 0 = 自動(REQ を同報)*/

#else /* VxWorks */

uint32_t    g_bfm_pci_bus        = 1;              /* PCIe のバス番号。例: 機材に合わせる */
uint32_t    g_bfm_pci_dev        = 0;              /* PCIe のデバイス番号 */
uint32_t    g_bfm_pci_func       = 0;              /* PCIe の function 番号 */
uint64_t    g_bfm_pcie_ecam_base = 0xE0000000ull;  /* ECAM の先頭。例: 機材の ECAM の位置に合わせる */
uint64_t    g_bfm_bar0_addr      = 0xC0000000ull;  /* BAR0 に割り当てるアドレス。例: BAR0 の大きさの境界に合わせる */
uint64_t    g_bfm_bar0_wc_va     = 0;              /* BAR0 の WC の別名。BSP が WC の別名を張ったらその先頭 */
uint32_t    g_bfm_pcie_flr       = 1;              /* FLR をかけるか。前回の実行の名残を消すため 1 */
uint32_t    g_bfm_pcie_mrrs      = 0;              /* Max Read Request Size。0 = 触らない */
const char *g_bfm_linux_pci_bdf  = 0;              /* 装置の PCI アドレス(VxWorks では使わない)*/

uint64_t g_bfm_dma_base = 0x40000000ull;            /* DMA 領域の先頭。例: OS が使わない物理メモリ */
uint64_t g_bfm_dma_size = 0x6000000ull;             /* DMA 領域の大きさ。96MiB(FW のページ 64MiB を含む)*/
uint64_t g_bfm_mr_base  = 0x46000000ull;            /* 公開メモリの先頭。例: DMA 領域の直後(1GiB で 0x86000000 まで)*/
uint64_t g_bfm_mr_size  = 0x40000000ull;            /* 公開メモリの大きさ。1GiB */

uint32_t g_bfm_local_ip     = 0xC0A8650Au;          /* 自分の IPv4。例: 192.168.101.10 */
uint8_t  g_bfm_local_mac[6] = {0, 0, 0, 0, 0, 0};   /* 自分の MAC。全部 0 = NIC の固有の MAC */
uint32_t g_bfm_peer_ip      = 0;                    /* 既定の相手の IPv4。未設定(bfm_connect() で渡す)*/
uint8_t  g_bfm_peer_mac[6]  = {0, 0, 0, 0, 0, 0};   /* 既定の相手の MAC。全部 0 = 自動(REQ を同報)*/

#endif

uint32_t g_bfm_fw_pages_max = 16384u;               /* FW に渡すページの上限。64MiB */
uint32_t g_bfm_stage_size   = 65536u;               /* 接続ごとの中継バッファの大きさ。64KiB */
uint32_t g_bfm_dif_buf_size = 131072u;              /* 接続ごとの T10-DIF 用の中継バッファの大きさ。2 面 x 64KiB */
uint32_t g_bfm_max_conn     = BFM_MAX_CONN;         /* 用意する接続の枠の数。64 */

uint16_t g_bfm_cm_port      = 18515u;               /* 接続の番号(待ち受けを見分ける)*/
uint8_t  g_bfm_path_mtu     = 3u;                   /* RoCE の経路 MTU。3 = 1024 バイト */
uint16_t g_bfm_port_mtu     = 0u;                   /* ポートの MTU。0 = 触らない */
uint32_t g_bfm_link_wait_ms = 10000u;               /* リンクが上がるのを待つ時間。10 秒 */

uint32_t g_bfm_bf_enable          = 1u;             /* BlueFlame を使う */
uint32_t g_bfm_scatter_cqe        = 1u;             /* 32 バイトまでの read / atomic の応答を CQE に書かせる */
uint8_t  g_bfm_ack_timeout        = 17u;            /* RC の ack timeout(4.096us x 2^17 = 約 0.54 秒)*/
uint8_t  g_bfm_retry_count        = 7u;             /* RC の再送回数(最大)*/
uint32_t g_bfm_op_timeout_ms      = 5000u;          /* 1 回の操作の完了を待つ上限。5 秒 */
uint32_t g_bfm_signal_every       = 32u;            /* bfm_write_post() で完了通知を求める間隔(WQE 32 個ごと)*/
uint32_t g_bfm_cpu_awake          = 1u;             /* CPU を深い C-state に入れない(相手からの read / atomic の遅延に効く)*/
uint64_t g_bfm_tsc_hz             = 0u;             /* TSC の周波数。0 = CPUID / MSR から求める */
uint32_t g_bfm_verbose            = 0u;             /* 経過を細かく表示する。0 = しない */
