#ifndef BFM_CONFIG_H
#define BFM_CONFIG_H

/* =====================================================================
 * 移植のときに設定する値は、すべてこのヘッダに集めてある。
 *
 * 実体(既定値)は src/bfm_config.c にある。既定値はプラットフォームごとに
 * 違う(Linux の試験環境と VxWorks)。VxWorks では、bfm_run() を最初に呼ぶ前に
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

/* 使う ConnectX のポートの数の上限。ConnectX-4〜7 は物理ポートごとに別の PCIe の function(PF)なので、
 * ポート 1 つ = PF 1 つ = 装置 1 つとして扱う(PCIe・HCA・GSI をポートごとに持つ)。 */
#ifndef BFM_MAX_PORT
#define BFM_MAX_PORT 2u
#endif

/* 1 つのポートに張る接続の数の上限(配列の大きさ)。1 接続 = RC QP 1 本(+ CQ・UAR 1 つずつ)。
 * 実際に張る数は g_bfm_port_conn[] で決める。 */
#ifndef BFM_MAX_PORT_CONN
#define BFM_MAX_PORT_CONN 16u
#endif

/* 接続の表の大きさ(全部のポートの合計。既定 2 x 16 = 32)。 */
#define BFM_MAX_CONN (BFM_MAX_PORT * BFM_MAX_PORT_CONN)

/* 多重度の上限(1 本の接続で、完了を待たずに出しておける write / read の数。bfm_write_async() /
 * bfm_read_async())。仕掛かり中の 1 個ごとに「枠」を 1 つ使う。tag の下位 6 ビットが枠の番号なので 64 まで。 */
#define BFM_QD_MAX 64u

/* 接続ごとにメモリに覚えておく状態遷移の数(bfm_conn_steplog() で表示する)。超えたら古いものから
 * 上書きする。1 件 24 バイトなので、表全体で BFM_MAX_CONN x BFM_STEPLOG_N x 24 バイト(既定 48KiB)。1 以上。 */
#ifndef BFM_STEPLOG_N
#define BFM_STEPLOG_N 64u
#endif

/* ---------------------------------------------------------------------
 * ポートごとの値(添字はポートの番号 0〜BFM_MAX_PORT-1)
 * ------------------------------------------------------------------- */

/* ポートに張る接続の数(0〜BFM_MAX_PORT_CONN。大きければ BFM_MAX_PORT_CONN にする)。
 * 0 ならそのポートは使わない(PCIe も触らない)。両方のノードで同じ値にすること
 * (接続 (ポート p, 番号 i) どうしが対になる)。 */
extern uint32_t g_bfm_port_conn[BFM_MAX_PORT];

/* ポートの PCIe の function(VxWorks のポート層が設定空間を引くのに使う)。 */
extern uint32_t g_bfm_pci_bus[BFM_MAX_PORT];
extern uint32_t g_bfm_pci_dev[BFM_MAX_PORT];
extern uint32_t g_bfm_pci_func[BFM_MAX_PORT];

/* BAR0(64bit・メモリ)に割り当てるアドレス。0 なら書き込まず、今入っている値を使う
 * (Linux はカーネルが割り当て済みなので 0)。BAR0 の大きさは装置が決める
 * (ConnectX-4〜7 は 32MiB〜。立ち上げのときに大きさを測って、境界が合わないと断る)。 */
extern uint64_t g_bfm_bar0_addr[BFM_MAX_PORT];

/* BAR0 全体を write-combining で張った別名の先頭(VxWorks)。BlueFlame の書き込み先。
 * 0 なら BlueFlame を使わず、ドアベル 8 バイトだけを UC の BAR0 へ書く(動くが遅い)。
 * Linux ではポート層が UAR のページだけを WC に張り替えるので使わない。 */
extern uint64_t g_bfm_bar0_wc_va[BFM_MAX_PORT];

/* Linux(VFIO)の試験環境だけで使う: ポートの PCI アドレス("0000:01:00.0")。 */
extern const char *g_bfm_linux_pci_bdf[BFM_MAX_PORT];

/* ポートの IPv4(ホスト順。0xC0A8650A = 192.168.101.10)。GID はここから作る
 * (::ffff:a.b.c.d)。IP のスタックは持たないので、ARP には答えない。 */
extern uint32_t g_bfm_local_ip[BFM_MAX_PORT];

/* ポートの MAC。全部 0 なら NIC の固有の MAC(vport の permanent_address)を使う。 */
extern uint8_t g_bfm_local_mac[BFM_MAX_PORT][6];

/* ポートの相手(相手のノードの、同じ番号のポート)の IPv4 と MAC。
 * **IPv4 の小さいほうのノードが掛け側(REQ を送る)、大きいほうが受け側になる**(自動で決まるので、
 * 両方のノードで同じプログラムを動かせる)。相手の MAC が全部 0 なら自動(掛け側は REQ を同報で送り、
 * REP に入った MAC を使う。受け側は REQ に入った MAC を使う)。同報が届かない網(L2 を越える経路など)
 * では MAC を書く。 */
extern uint32_t g_bfm_peer_ip[BFM_MAX_PORT];
extern uint8_t  g_bfm_peer_mac[BFM_MAX_PORT][6];

/* ---------------------------------------------------------------------
 * PCIe(全部のポートで共通)
 * ------------------------------------------------------------------- */

/* PCIe の ECAM(拡張設定空間)の先頭。VxWorks のポート層はここを直接読み書きする
 * (bus << 20 | dev << 15 | func << 12 | off)。0 だと立ち上げで断る。Linux では使わない。 */
extern uint64_t g_bfm_pcie_ecam_base;

/* 1 = 立ち上げの前に FLR(Function Level Reset)をかける。前回の実行で NIC が
 * 動いたまま(DMA を出したまま)のことがあるので VxWorks では 1 にしておく。
 * Linux は vfio-pci が open のときにリセットするので 0。 */
extern uint32_t g_bfm_pcie_flr;

/* PCIe の Max Read Request Size(バイト: 128〜4096)。0 なら触らない。 */
extern uint32_t g_bfm_pcie_mrrs;

/* ---------------------------------------------------------------------
 * メモリ(どちらも 4KiB 境界、大きさは 4KiB の倍数。全部のポートで共有)
 * ------------------------------------------------------------------- */

/* ドライバが使う DMA 領域。コマンドキュー・メールボックス・FW に渡すページ・
 * EQ / CQ / QP のリング・接続ごとの中継バッファを、ポートごとにここから静的に切り出す。
 * 足りなければ立ち上げ(bfm_run())が必要な大きさを表示して失敗する。 */
extern uint64_t g_bfm_dma_base;
extern uint64_t g_bfm_dma_size;

/* 相手に見せるメモリ(相手が RDMA の write / read / atomic で直接触る範囲)。
 * ポートごとにこの範囲だけを許した MKey を作り、その rkey と先頭アドレスを接続のときに相手へ渡す
 * (どのポート・どの接続から来ても、触るのはこの同じメモリ)。
 * 既定は 1GiB。大きさは 64bit で扱う(MKey は PA モードなので、大きくしても NIC の資源は増えない)。
 * VxWorks では、この範囲が OS に使われない RAM であること。Linux では起動のたびにこの大きさを
 * 確保して VFIO に登録する(2MiB の hugepage が足りなければ 4KiB のページで張る)。 */
extern uint64_t g_bfm_mr_base;
extern uint64_t g_bfm_mr_size;

/* FW(ConnectX)へ MANAGE_PAGES で渡すページ(4KiB)の上限(ポートごと)。DMA 領域から取る。
 * 実測の要求量は ConnectX-4 Lx で約 2,500、ConnectX-4 で約 4,500。ConnectX-7 は
 * もっと多いはずなので余裕を持たせてある。足りなければ立ち上げで要求量を表示する。 */
extern uint32_t g_bfm_fw_pages_max;

/* 接続ごとの中継バッファの大きさ。inline に収まらない write の送り元と、
 * 32 バイトを超える read の着地点。1 回の操作の長さの上限にもなる(超える分は分割)。
 * 接続 1 本で DMA 領域を、これと T10-DIF 用の中継バッファと QP・CQ のリング(約 20KiB)だけ使う。 */
extern uint32_t g_bfm_stage_size;

/* 接続ごとの T10-DIF 用の中継バッファの大きさ(2 面に分けて交互に使う)。
 * 1 面 = この半分で、1 回の RDMA で運ぶブロック数 = 1 面 / 520(既定 128KiB なら 126 ブロック)。
 * 普通の write / read の中継バッファとは別なので、DIF の転送の途中でも普通の操作を出せる。 */
extern uint32_t g_bfm_dif_buf_size;

/* 多重度(1〜BFM_QD_MAX。0 か上限より大きければ BFM_QD_MAX)。1 本の接続で同時に仕掛かり中にできる
 * bfm_write_async() / bfm_read_async() の数。これを超えて出そうとすると、完了を刈り取って枠を空けてから出す。 */
extern uint32_t g_bfm_qdepth;

/* 多重度の枠ごとの中継バッファの大きさ(バイト。64 の倍数に切り上げる)。inline に収まらない
 * bfm_write_async() の送り元と、bfm_read_async() の着地点で、1 回の非同期の操作の長さの上限になる。
 * 接続 1 本で DMA 領域を g_bfm_qdepth 倍使う(既定 64 x 4KiB = 256KiB)。 */
extern uint32_t g_bfm_async_buf_size;

/* 非同期の操作(bfm_write_async() / bfm_read_async())の時間切れ(ミリ秒)。出してからこの時間を過ぎても
 * ACK・応答が無ければ、その接続をエラーにして BFM_E_TIMEOUT を返す(枠の空き待ち・bfm_test()・bfm_check())。 */
extern uint32_t g_bfm_async_timeout_ms;

/* ---------------------------------------------------------------------
 * ネットワーク(RoCE v2、IPv4。全部のポートで共通)
 * ------------------------------------------------------------------- */

/* 接続の番号の起点(待ち受けを見分ける番号)。接続 (ポート p, 番号 i) は g_bfm_cm_port + i を使う。
 * 両方のノードで同じ値にすること。 */
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

/* 掛け側が、REP が来なければ REQ を送り直す間隔(ミリ秒)。相手のノードが立ち上がるまで
 * 送り直し続ける(時間切れにしない)。 */
extern uint32_t g_bfm_req_retry_ms;

/* ---------------------------------------------------------------------
 * 動き
 * ------------------------------------------------------------------- */

/* 1 = BlueFlame を使う(WC の窓が取れたときだけ効く)。0 = 8 バイトのドアベルだけ。 */
extern uint32_t g_bfm_bf_enable;

/* 1 = 32 バイトまでの read / atomic の応答を CQE に書かせる(scatter to CQE)。 */
extern uint32_t g_bfm_scatter_cqe;

/* RC の再送: ack timeout(4.096us x 2^N)と再送回数(0〜7)。NIC がハードウェアで再送する。
 * 途中のフレームが抜けたときは、受け側が番号の飛びに気付いて NAK を返すので、すぐ(往復 1 回で)送り直す。
 * 後ろに続くもののない最後のフレームが抜けたときは、ack timeout が過ぎてから送り直す。0 は「待ち続ける」(送り直さない)。
 * **ConnectX-4 Lx(FW 14.29 / 14.32)では、NIC が実際に待つ時間に約 0.37〜0.40 秒の下限がある**(4〜16 のどれを設定しても
 * 送り直す間隔は約 370〜400ms。17 なら 0.54 秒以上。試験 run -z の [10] D で測った)。そのため、最後のフレームが抜けたときは、
 * 非同期の操作の時間切れ(g_bfm_async_timeout_ms、既定 100ms)が先に来て接続がエラーになる(張り直すと使える)。
 * 既定の 12 は、下限の間隔で送り直させるための値(設定上は 4.096us x 2^12 = 約 16.8ms)。
 * 再送が尽きると(既定 7 回。下限の間隔で約 3 秒)、NIC は TRANSPORT_RETRY_EXC のエラー完了を返す(BFM_E_RETRY)。 */
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

/* TSC の周波数(Hz)。0 なら CPUID(15h・16h)か MSR_PLATFORM_INFO から求める(port/x86/bfm_x86.c)。
 * VxWorks の時刻と待ちは TSC で作るので、求められない CPU ではここに入れる。Linux では表示にだけ使う。 */
extern uint64_t g_bfm_tsc_hz;

/* 1 = 立ち上げと接続の経過を細かく表示する。 */
extern uint32_t g_bfm_verbose;

#endif /* BFM_CONFIG_H */
