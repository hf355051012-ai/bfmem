/* bfm_config.h の変数の実体と既定値。
 *
 * 既定値はプラットフォームで分ける。Linux(VFIO)の値は OptiPlex の ConnectX-4 Lx を
 * Precision と直結した試験環境のもの(繋いであるのは 1 本だけなので、ポート 1 は使わない)。
 * VxWorks の値は置き場所の例で、そのままでは動かない(BAR0 のアドレス・メモリの位置・IP は
 * 機材に合わせて必ず書き換えること)。各変数の詳しい意味は bfm_config.h のコメント。 */
#include "bfm_config.h"

#if defined(BFM_PORT_LINUX)

uint32_t    g_bfm_port_conn[BFM_MAX_PORT]     = {16u, 0u};            /* ポートごとの接続の数。ポート 1 は使わない */
uint32_t    g_bfm_pci_bus[BFM_MAX_PORT]       = {0u, 0u};             /* PCIe のバス番号(Linux では使わない)*/
uint32_t    g_bfm_pci_dev[BFM_MAX_PORT]       = {0u, 0u};             /* PCIe のデバイス番号(Linux では使わない)*/
uint32_t    g_bfm_pci_func[BFM_MAX_PORT]      = {0u, 0u};             /* PCIe の function 番号(Linux では使わない)*/
uint64_t    g_bfm_bar0_addr[BFM_MAX_PORT]     = {0u, 0u};             /* BAR0 に割り当てるアドレス。カーネルが割り当てた値を使う */
uint64_t    g_bfm_bar0_wc_va[BFM_MAX_PORT]    = {0u, 0u};             /* BAR0 の WC の別名。ポート層が UAR のページを WC に張り替える */
const char *g_bfm_linux_pci_bdf[BFM_MAX_PORT] = {"0000:01:00.1",      /* ポート 0 の PCI アドレス(OptiPlex の ConnectX-4 Lx の PF1)*/
                                                 "0000:01:00.0"};     /* ポート 1 の PCI アドレス(OptiPlex の PF0)*/
uint32_t    g_bfm_local_ip[BFM_MAX_PORT]      = {0xC0A8650Bu,         /* ポート 0 の IPv4。192.168.101.11 */
                                                 0xC0A8660Bu};        /* ポート 1 の IPv4。192.168.102.11 */
uint8_t     g_bfm_local_mac[BFM_MAX_PORT][6]  = {{0}, {0}};           /* ポートの MAC。全部 0 = NIC の固有の MAC */
uint32_t    g_bfm_peer_ip[BFM_MAX_PORT]       = {0xC0A86515u,         /* ポート 0 の相手の IPv4。192.168.101.21 */
                                                 0xC0A86615u};        /* ポート 1 の相手の IPv4。192.168.102.21 */
uint8_t     g_bfm_peer_mac[BFM_MAX_PORT][6]   = {{0}, {0}};           /* 相手の MAC。全部 0 = 自動(REQ を同報)*/

uint64_t    g_bfm_pcie_ecam_base = 0;              /* ECAM の先頭(Linux では使わない)*/
uint32_t    g_bfm_pcie_flr       = 0;              /* FLR をかけるか。vfio-pci が open のときにリセットする */
uint32_t    g_bfm_pcie_mrrs      = 0;              /* Max Read Request Size。0 = 触らない */

/* 仮想アドレス = IOVA としてこの位置に張る(4GiB より上。x86 の MSI の窓 0xfee00000 を避ける)。 */
uint64_t g_bfm_dma_base = 0x100000000ull;           /* DMA 領域の先頭 */
uint64_t g_bfm_dma_size = 0xA000000ull;             /* DMA 領域の大きさ。160MiB(FW のページ 64MiB x 2 ポートを含む)*/
uint64_t g_bfm_mr_base  = 0x110000000ull;           /* 公開メモリの先頭 */
uint64_t g_bfm_mr_size  = 0x40000000ull;            /* 公開メモリの大きさ。1GiB */

#else /* VxWorks */

uint32_t    g_bfm_port_conn[BFM_MAX_PORT]     = {16u, 16u};           /* ポートごとの接続の数。2 ポート x 16 本 */
uint32_t    g_bfm_pci_bus[BFM_MAX_PORT]       = {1u, 1u};             /* PCIe のバス番号。例: 機材に合わせる */
uint32_t    g_bfm_pci_dev[BFM_MAX_PORT]       = {0u, 0u};             /* PCIe のデバイス番号 */
uint32_t    g_bfm_pci_func[BFM_MAX_PORT]      = {0u, 1u};             /* PCIe の function 番号(ポート 0 = PF0、ポート 1 = PF1)*/
uint64_t    g_bfm_bar0_addr[BFM_MAX_PORT]     = {0xC0000000ull,       /* BAR0 に割り当てるアドレス。例: BAR0 の大きさの境界に合わせる */
                                                 0xC4000000ull};
uint64_t    g_bfm_bar0_wc_va[BFM_MAX_PORT]    = {0u, 0u};             /* BAR0 の WC の別名。BSP が WC の別名を張ったらその先頭 */
const char *g_bfm_linux_pci_bdf[BFM_MAX_PORT] = {0, 0};               /* PCI アドレス(VxWorks では使わない)*/
uint32_t    g_bfm_local_ip[BFM_MAX_PORT]      = {0xC0A8650Au,         /* ポート 0 の IPv4。例: 192.168.101.10 */
                                                 0xC0A8660Au};        /* ポート 1 の IPv4。例: 192.168.102.10 */
uint8_t     g_bfm_local_mac[BFM_MAX_PORT][6]  = {{0}, {0}};           /* ポートの MAC。全部 0 = NIC の固有の MAC */
uint32_t    g_bfm_peer_ip[BFM_MAX_PORT]       = {0xC0A86514u,         /* ポート 0 の相手の IPv4。例: 192.168.101.20 */
                                                 0xC0A86614u};        /* ポート 1 の相手の IPv4。例: 192.168.102.20 */
uint8_t     g_bfm_peer_mac[BFM_MAX_PORT][6]   = {{0}, {0}};           /* 相手の MAC。全部 0 = 自動(REQ を同報)*/

uint64_t    g_bfm_pcie_ecam_base = 0xE0000000ull;  /* ECAM の先頭。例: 機材の ECAM の位置に合わせる */
uint32_t    g_bfm_pcie_flr       = 1;              /* FLR をかけるか。前回の実行の名残を消すため 1 */
uint32_t    g_bfm_pcie_mrrs      = 0;              /* Max Read Request Size。0 = 触らない */

uint64_t g_bfm_dma_base = 0x40000000ull;            /* DMA 領域の先頭。例: OS が使わない物理メモリ */
uint64_t g_bfm_dma_size = 0xA000000ull;             /* DMA 領域の大きさ。160MiB(FW のページ 64MiB x 2 ポートを含む)*/
uint64_t g_bfm_mr_base  = 0x4A000000ull;            /* 公開メモリの先頭。例: DMA 領域の直後(1GiB で 0x8A000000 まで)*/
uint64_t g_bfm_mr_size  = 0x40000000ull;            /* 公開メモリの大きさ。1GiB */

#endif

uint32_t g_bfm_fw_pages_max = 16384u;               /* FW に渡すページの上限(ポートごと)。64MiB */
uint32_t g_bfm_stage_size   = 65536u;               /* 接続ごとの中継バッファの大きさ。64KiB */
uint32_t g_bfm_dif_buf_size = 131072u;              /* 接続ごとの T10-DIF 用の中継バッファの大きさ。2 面 x 64KiB */
uint32_t g_bfm_dif_clmul    = 1u;                   /* DIF の CRC16 に PCLMULQDQ を使う(CPU にあれば)*/
uint32_t g_bfm_qdepth       = BFM_QD_MAX;           /* 多重度。64 */
uint32_t g_bfm_async_buf_size   = 4096u;            /* 多重度の枠ごとの中継バッファ。4KiB(非同期の操作の長さの上限)*/
uint32_t g_bfm_async_timeout_ms = 100u;             /* 非同期の操作の時間切れ。出してから 100ms */

uint16_t g_bfm_cm_port      = 18515u;               /* 接続の番号の起点(接続 i は +i)*/
uint8_t  g_bfm_path_mtu     = 3u;                   /* RoCE の経路 MTU。3 = 1024 バイト */
uint16_t g_bfm_port_mtu     = 0u;                   /* ポートの MTU。0 = 触らない */
uint32_t g_bfm_link_wait_ms = 10000u;               /* リンクが上がるのを待つ時間。10 秒 */
uint32_t g_bfm_req_retry_ms = 1000u;                /* REP が来なければ REQ を送り直す間隔。1 秒 */

uint32_t g_bfm_bf_enable          = 1u;             /* BlueFlame を使う */
uint32_t g_bfm_scatter_cqe        = 1u;             /* 32 バイトまでの read / atomic の応答を CQE に書かせる */
uint8_t  g_bfm_ack_timeout        = 12u;            /* RC の ack timeout(設定上 4.096us x 2^12 = 約 16.8ms。ConnectX-4 Lx は実際には約 0.37 秒以上待つ)*/
uint8_t  g_bfm_retry_count        = 7u;             /* RC の再送回数(最大)*/
uint32_t g_bfm_op_timeout_ms      = 5000u;          /* 1 回の操作の完了を待つ上限。5 秒 */
uint32_t g_bfm_signal_every       = 32u;            /* bfm_write_post() で完了通知を求める間隔(WQE 32 個ごと)*/
uint32_t g_bfm_fw_allow_no_psid_check = 0u;         /* FW が PSID を確かめない装置へは FW を書かない */
uint32_t g_bfm_cpu_awake          = 1u;             /* CPU を深い C-state に入れない(相手からの read / atomic の遅延に効く)*/
uint64_t g_bfm_tsc_hz             = 0u;             /* TSC の周波数。0 = CPUID / MSR から求める */
uint32_t g_bfm_verbose            = 0u;             /* 経過を細かく表示する。0 = しない */
