#ifndef BFM_CONFIG_H
#define BFM_CONFIG_H

/* =====================================================================
 * 移植のときに設定する値は、すべてこのヘッダに集めてある。
 *
 * 実体(既定値)は src/bfm_config.c にある。既定値はプラットフォームごとに
 * 違う(Linux の試験環境と VxWorks)。VxWorks では、bfm_init() を呼ぶ前に
 * アプリケーション(または BSP の初期化)からこれらの変数へ代入すること。
 *
 * アドレスはすべて「論理アドレス = 物理アドレス」として扱う(VxWorks の
 * ページテーブルがそう張られている前提)。Linux の試験環境では、ポート層が
 * 同じ値を仮想アドレスと IOVA の両方に使うので、同じ前提がそのまま成り立つ
 * (port/linux_vfio/port_linux.c)。
 * ===================================================================== */

#include <stdint.h>

/* ---------------------------------------------------------------------
 * コンパイル時に決める値(配列の大きさになるので変数にはできない)
 * ------------------------------------------------------------------- */

/* 同時に張れる接続の数。1 接続 = RC QP 1 本(+ CQ・UAR 1 つずつ)。 */
#ifndef BFM_MAX_CONN
#define BFM_MAX_CONN 4u
#endif

/* ---------------------------------------------------------------------
 * PCIe
 * ------------------------------------------------------------------- */

/* 使う PCIe の function(VxWorks のポート層が設定空間を引くのに使う)。 */
extern uint32_t g_bfm_pci_bus;
extern uint32_t g_bfm_pci_dev;
extern uint32_t g_bfm_pci_func;

/* PCIe の ECAM(拡張設定空間)の先頭。VxWorks のポート層はここを直接読み書きする
 * (bus << 20 | dev << 15 | func << 12 | off)。0 だと bfm_init() が断る。Linux では使わない。 */
extern uint64_t g_bfm_pcie_ecam_base;

/* BAR0(64bit・メモリ)に割り当てるアドレス。0 なら書き込まず、今入っている値を使う
 * (Linux はカーネルが割り当て済みなので 0)。BAR0 の大きさは装置が決める
 * (ConnectX-4〜7 は 32MiB〜。bfm_init() が大きさを測って、境界が合わないと断る)。 */
extern uint64_t g_bfm_bar0_addr;

/* BAR0 全体を write-combining で張った別名の先頭(VxWorks)。BlueFlame の書き込み先。
 * 0 なら BlueFlame を使わず、ドアベル 8 バイトだけを UC の BAR0 へ書く(動くが遅い)。
 * Linux ではポート層が UAR のページだけを WC に張り替えるので使わない。 */
extern uint64_t g_bfm_bar0_wc_va;

/* 1 = 立ち上げの前に FLR(Function Level Reset)をかける。前回の実行で NIC が
 * 動いたまま(DMA を出したまま)のことがあるので VxWorks では 1 にしておく。
 * Linux は vfio-pci が open のときにリセットするので 0。 */
extern uint32_t g_bfm_pcie_flr;

/* PCIe の Max Read Request Size(バイト: 128〜4096)。0 なら触らない。 */
extern uint32_t g_bfm_pcie_mrrs;

/* Linux(VFIO)の試験環境だけで使う: 装置の PCI アドレス("0000:01:00.0")。 */
extern const char *g_bfm_linux_pci_bdf;

/* ---------------------------------------------------------------------
 * メモリ(どちらも 4KiB 境界、大きさは 4KiB の倍数)
 * ------------------------------------------------------------------- */

/* ドライバが使う DMA 領域。コマンドキュー・メールボックス・FW に渡すページ・
 * EQ / CQ / QP のリング・接続ごとの中継バッファをここから静的に切り出す。
 * 足りなければ bfm_init() が必要な大きさを表示して失敗する。 */
extern uint64_t g_bfm_dma_base;
extern uint64_t g_bfm_dma_size;

/* 相手に見せるメモリ(相手が RDMA の write / read / atomic で直接触る範囲)。
 * この範囲だけを許した MKey を作り、その rkey と先頭アドレスを接続のときに相手へ渡す。 */
extern uint64_t g_bfm_mr_base;
extern uint64_t g_bfm_mr_size;

/* FW(ConnectX)へ MANAGE_PAGES で渡すページ(4KiB)の上限。DMA 領域から取る。
 * 実測の要求量は ConnectX-4 Lx で約 2,500、ConnectX-4 で約 4,500。ConnectX-7 は
 * もっと多いはずなので余裕を持たせてある。足りなければ bfm_init() が要求量を表示する。 */
extern uint32_t g_bfm_fw_pages_max;

/* 接続ごとの中継バッファの大きさ。inline に収まらない write の送り元と、
 * 32 バイトを超える read の着地点。1 回の操作の長さの上限にもなる(超える分は分割)。 */
extern uint32_t g_bfm_stage_size;

/* ---------------------------------------------------------------------
 * ネットワーク(RoCE v2、IPv4)
 * ------------------------------------------------------------------- */

/* 自分の IPv4(ホスト順。0xC0A8650A = 192.168.101.10)。GID はここから作る
 * (::ffff:a.b.c.d)。IP のスタックは持たないので、ARP には答えない。 */
extern uint32_t g_bfm_local_ip;

/* 自分の MAC。全部 0 なら NIC の固有の MAC(vport の permanent_address)を使う。 */
extern uint8_t g_bfm_local_mac[6];

/* 既定の相手(bfm_connect() の peer_ip が 0 / peer_mac が NULL のときに使う)。
 * ARP を使わないので、掛け側は相手の MAC が必ず要る(受け側は REQ に入った MAC を使う)。 */
extern uint32_t g_bfm_peer_ip;
extern uint8_t  g_bfm_peer_mac[6];

/* 接続の番号(待ち受けを見分ける番号。bfm_connect() と bfm_accept() で同じ値にする)。 */
extern uint16_t g_bfm_cm_port;

/* RoCE の経路 MTU(IB の符号: 1=256 2=512 3=1024 4=2048 5=4096)。
 * 接続の両端で小さいほうになる。Ethernet の MTU が 4200 未満なら 3(1024)。 */
extern uint8_t g_bfm_path_mtu;

/* ポートの MTU。IP の MTU(Linux の ip link set mtu と同じ意味。9000 など)で書く。0 なら触らない。
 * PMTU レジスタにはヘッダの分(22 バイト)を足したフレーム長で入れる。
 * RoCE の経路 MTU を 4096 にするなら、両端ともここを 4200 以上(普通は 9000)にする。 */
extern uint16_t g_bfm_port_mtu;

/* リンクが上がるのを待つ時間(ミリ秒)。 */
extern uint32_t g_bfm_link_wait_ms;

/* ---------------------------------------------------------------------
 * 動き
 * ------------------------------------------------------------------- */

/* 1 = BlueFlame を使う(WC の窓が取れたときだけ効く)。0 = 8 バイトのドアベルだけ。 */
extern uint32_t g_bfm_bf_enable;

/* 1 = 32 バイトまでの read / atomic の応答を CQE に書かせる(scatter to CQE)。 */
extern uint32_t g_bfm_scatter_cqe;

/* RC の再送: ack timeout(4.096us x 2^N)と再送回数(0〜7)。 */
extern uint8_t g_bfm_ack_timeout;
extern uint8_t g_bfm_retry_count;

/* 1 回の操作の完了を待つ上限(ミリ秒)。超えると接続をエラーにする。 */
extern uint32_t g_bfm_op_timeout_ms;

/* bfm_write_post() で完了通知(CQE)を求める間隔(WQE の数)。 */
extern uint32_t g_bfm_signal_every;

/* 1 = 動いている間、CPU を深い省電力状態(パッケージの C-state)に入れない。
 * **相手からの read / atomic の遅延に効く。** 受け側の NIC はホストのメモリを読んで応答するので、
 * CPU が深く眠っているとメモリ側が起きるまで待たされる(ConnectX-4 Lx + Core i3-8100 の受け側で
 * read / FAA が約 300ns 遅くなった)。write は相手のメモリを読まないので効かない。
 * Linux: /dev/cpu_dma_latency に 0 を書いて保持する。VxWorks: ポート層では何もしないので、
 * BIOS / BSP で深い C-state を止めること(README「VxWorks への移植」)。 */
extern uint32_t g_bfm_cpu_awake;

/* 1 = 立ち上げと接続の経過を細かく表示する。 */
extern uint32_t g_bfm_verbose;

#endif /* BFM_CONFIG_H */
