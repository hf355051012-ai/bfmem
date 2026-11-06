# bfmem

ConnectX(mlx5)の **BlueFlame** を使って、ノード間で相手のメモリへ直接
**write / read / atomic(CAS・FAA)** を打つためのライブラリ。RoCE v2 の RC QP を 1 本張り、
WQE を UAR の BlueFlame の窓へ MMIO で書き込んで送る。相手の CPU は何もしない(相手の NIC が応答する)。

- 対象は **VxWorks(論理アドレス = 物理アドレス)どうし**。設計と試験は **x86-64 Linux + VFIO** で行い、
  プラットフォームに依存する部分は移植層(`port/`)に分けてある。
- `vfio_nvme` の mlx5 ドライバ(ConnectX-4 / 4 Lx の実機で確認済み)から、この用途に要る部分だけを
  取り出して作り直した。**NVMe・NVMe/TCP・NVMe-oF・iSCSI・iSER・TCP/IP のスタックは一切含まない。**
- 試験できるのは今のところ ConnectX-4 系だけ。**ConnectX-7 でも動くように書いてある**(下の「ConnectX-7 への備え」)が、
  実機では確かめていない。

## 構成

| 場所 | 中身 |
|---|---|
| `include/bfm_config.h` | **移植のときに設定する変数を全部ここに集めてある**(実体と既定値は `src/bfm_config.c`)|
| `include/bfm.h` | アプリケーションが使う API |
| `include/bfm_port.h` | 移植層の関数の宣言と、CPU ごとのバリア・バイト順の道具 |
| `src/bfm_core.c` | `bfm_init()` / `bfm_shutdown()` / `bfm_poll()` |
| `src/bfm_pci.c` | PCIe の設定空間の設定(FLR・BAR0・Bus Master・MRRS)|
| `src/mlx5_cmd.c` | コマンドキュー、FW へのページの受け渡し、EQ |
| `src/mlx5_hca.c` | HCA の立ち上げ、UAR・PD・MKey・CQ、ポート、RoCE のアドレス |
| `src/mlx5_qp.c` | QP(RC と GSI)の作成と状態遷移 |
| `src/bfm_cm.c` | 接続の確立と切断(GSI の上の IB CM)|
| `src/bfm_ops.c` | データ経路(BlueFlame で WQE を送る、完了を拾う)|
| `src/bfm_mem.c` | DMA 領域からの静的な切り出し |
| `port/linux_vfio/port_linux.c` | 移植層: Linux + VFIO(試験環境)|
| `port/vxworks/port_vxworks.c` | 移植層: VxWorks(**未検証**)|
| `test/bfm_test.c` | 試験プログラム(server / client / info)|
| `tools/bfm_peer.c` | 試験の相手(Linux のカーネルの mlx5 + librdmacm)|
| `doc/spec/bfmem_spec.adoc` | **関数仕様書**(全ソースの全関数。表・フロー図・関係図。図は `doc/spec/images/*.drawio.svg`)。`doc/spec/gen.py` で作る。HTML 版は `doc/spec/bfmem_spec.html`(図を埋め込んだ 1 ファイル。asciidoctor があれば gen.py が一緒に作る) |

## API

```c
#include "bfm.h"

bfm_init();                                   /* PCIe・HCA・RoCE の準備 */
bfm_conn_t *c = bfm_accept(0, 0);             /* 受け側(ポートは g_bfm_cm_port)*/
bfm_conn_t *c = bfm_connect(ip, mac, 0, 0);   /* 掛け側 */

bfm_write(c, off, src, len);                  /* 相手の公開メモリ + off へ。ACK まで待つ */
bfm_write_post(c, off, src, len);             /* 待たない(inline に収まる長さだけ)*/
bfm_flush(c);                                 /* bfm_write_post() の分を待つ */
bfm_read(c, off, dst, len);
bfm_cas(c, off, expect, desired, &old);       /* 8 バイト、off は 8 の倍数 */
bfm_faa(c, off, add, &old);

bfm_poll();                                   /* ときどき呼ぶ(切断要求・FW のページ要求)*/
bfm_disconnect(c);
bfm_shutdown();
```

- 1 本の接続で、**両方のノードが互いのメモリを触れる**(接続のときに両方向へ公開メモリの情報を渡す)。
- `off` は相手が公開した範囲(`g_bfm_mr_base` から `g_bfm_mr_size`)の先頭からのオフセット。
  範囲の外は手元で断り(`BFM_E_PARAM`)、相手の NIC も MKey の範囲で断る。
- `src` / `dst` はどこにあってもよい(NIC が直接読み書きするのは中継バッファで、CPU が写す)。
- **スレッド安全ではない。** 1 つの接続を複数のタスクから同時に使わないこと。

## 設計

### VFIO で作って VxWorks へ移す

ライブラリの本体(`src/`)は OS の機能を何も使わない。使うのは移植層(`include/bfm_port.h`)の
10 個ほどの関数だけ: 設定空間の読み書き、BAR0 を張る、BAR0 の一部を write-combining で張る、
時刻、待つ、表示、キャッシュの操作。

**アドレスの約束: NIC に渡すアドレス = CPU のポインタの値。** VxWorks は論理 = 物理なので
そのまま成り立つ。Linux では、移植層が DMA 領域と公開メモリを `g_bfm_dma_base` / `g_bfm_mr_base`
の仮想アドレスにそのまま張り(`MAP_FIXED_NOREPLACE`)、**同じ値を IOVA にして** VFIO に登録する。
これで本体は物理アドレスへの変換を一切持たずに、Linux でも VxWorks と同じ書き方で動く。

### メモリ

使うメモリは 2 つの区画だけで、どちらも変数で位置と大きさを決める。

| 区画 | 変数 | 中身 |
|---|---|---|
| DMA 領域 | `g_bfm_dma_base` / `g_bfm_dma_size` | コマンドキュー・メールボックス・**FW に渡すページ**・EQ / CQ / QP のリング・接続ごとの中継バッファ。`bfm_init()` のときに静的に切り出す(返さない)|
| 公開メモリ | `g_bfm_mr_base` / `g_bfm_mr_size` | 相手が直接触る範囲。この範囲だけを許した MKey(PA モード)を作る |

既定の大きさは DMA 領域 96MiB(そのうち FW のページが `g_bfm_fw_pages_max` = 16,384 ページ = 64MiB)。
足りなければ `bfm_init()` が必要な量を表示して失敗する。

### BlueFlame

1 個の操作 = 1 個の WQE。WQE を手元で組み、SQ のリング(ホストメモリ)へ写し、doorbell record を
進めてから、**同じ WQE を UAR の BlueFlame の窓(UAR ページ + 0x800)へ 64 ビットずつ書き**、
write-combining のバッファを吐き出させる(x86 は `sfence`)。窓は WC なので 64 バイトが 1 個の
PCIe の書き込みにまとまり、NIC はドアベルと一緒に WQE を受け取る(リングへ読みに来ない)。

- **write は 220 バイトまでデータを WQE に埋め込む(inline)。** WQE は最大 256 バイト(窓 1 面の大きさ)。
  データを読みに来る DMA も無くなるので、write は MMIO の書き込み 1 回で出ていく。
- 窓は 2 面あり、交互に使う(`log_bf_reg_size` の半分ずつ。rdma-core の mlx5 と同じ)。
- 窓へ書いたものが 1 回にまとまらなかったとき、NIC はそれを捨ててリングから読むので、結果は同じ
  (遅くなるだけ)。**WC が取れないときは 8 バイトのドアベルだけで動く。**
- **read と atomic は、32 バイトまでの応答を CQE に書かせる**(QP の `cs_req`、`g_bfm_scatter_cqe`)。
  そのため read と atomic は必ず完了通知を求める(通知なしの read は QP をエラーにする。vfio_nvme で確認)。
- 1 接続に UAR を 1 つ割り当てるので、接続ごとに窓が別になる(接続どうしで排他は要らない)。

Linux で BAR0 の一部を WC にするには手順が要る(`port/linux_vfio/port_linux.c` の冒頭)。
VFIO の fd から BAR を張ると vfio-pci がカーネルの中でも BAR 全体を UC で登録し、x86 の PAT が
同じ物理ページに WC を許さなくなる(黙って UC- に格下げされる)。そのため BAR0 は sysfs の
`resource0` から張り、UAR のページだけを `resource0_wc` で張り替える。

### 接続(IB CM)

RC QP を繋ぐには、相手の QP 番号・開始 PSN と、公開メモリのアドレス・rkey を交換する必要がある。
IP のスタックを持たないので、交換は **RoCE の GSI(QP1)で IB CM の REQ / REP / RTU** を使う
(Linux の rdma_cm と同じ形なので、試験では Linux の相手と繋がる)。公開メモリの情報と自分の MAC は
private data に入れる(REQ は rdma_cm の `cma_hdr` 36 バイトの後ろ、REP は先頭)。

```
[0..3] 'BFM1'  [4..5] 版 = 1  [6..7] 旗(bit0 = MAC あり)
[8..15] 公開メモリの先頭  [16..19] rkey  [20..27] 大きさ  [28..33] MAC   (ビッグエンディアン)
```

**ARP を使わない。** 掛け側は相手の IP と MAC を引数(か `g_bfm_peer_ip` / `g_bfm_peer_mac`)で知り、
受け側は相手の MAC を REQ の private data から知る。GID は IP から作る(`::ffff:a.b.c.d`、RoCE v2)。

### atomic のバイト順

立ち上げのときに、対応していれば **8 バイトの atomic をホストのバイト順で扱う設定**にする
(`atomic_req_8B_endianness_mode`、Linux の mlx5_core と同じ。`g_bfm_atomic_host_endian`)。
`bfm_cas()` / `bfm_faa()` の `old` はこの設定に合わせてホストの数に直して返す。
相手のメモリの中の並びは**相手の HCA の設定で決まる**ので、両方を bfmem にしておけば揃う。
試験プログラムの `[4]` が、相手のメモリの並びを読んで表示する。

### ConnectX-7 への備え(実機では未確認)

| 違い | 対応 |
|---|---|
| 能力(HCA cap)の構造が長い | QUERY / SET_HCA_CAP は 4,096 バイトまるごと読み書きする(Linux と同じ)|
| FW の起動が遅い | 初期化待ちの上限を 120 秒にした |
| FW に渡すページが多い | 上限を変数(`g_bfm_fw_pages_max`)にし、足りなければ要求量を表示して止まる |
| 時刻の形式を実時間だけにできる | RoCE の能力 `qp_ts_format` が 0 でなければ QP の `ts_format` を DEFAULT にする(Linux の `mlx5_get_qp_default_ts()` と同じ)|
| `num_vhca_ports` が 0 でない | SET_ROCE_ADDRESS にポート番号を入れる |
| BlueFlame の窓の大きさ | `log_bf_reg_size` から求める(想定外の値なら 512 として警告)|
| atomic の対応 | atomic の能力から QP の `atomic_mode` を決める(Linux の `get_atomic_mode()` と同じ規則)。断られたら atomic を受けない形で繋ぐ |
| 装置の番号 | 0x1013 / 0x1015(CX-4 / Lx)、0x1017 / 0x1019(CX-5)、0x101b / 0x101d / 0x101f(CX-6)、**0x1021(CX-7)** |

## VxWorks への移植

### 変数(`include/bfm_config.h`)

`bfm_init()` の前に代入する。既定値は `src/bfm_config.c`(VxWorks の値は置き場所の例で、
**そのままでは動かない**)。

| 変数 | 意味 |
|---|---|
| `g_bfm_pci_bus` / `_dev` / `_func` | 使う PCIe の function |
| `g_bfm_pcie_ecam_base` | ECAM の先頭(設定空間を直接読み書きする)|
| `g_bfm_bar0_addr` | BAR0 に書き込むアドレス(BAR0 の大きさの境界に合わせる。0 なら今の値)|
| `g_bfm_bar0_wc_va` | BAR0 全体を WC で張った別名の先頭(0 なら BlueFlame なし)|
| `g_bfm_pcie_flr` | 立ち上げの前に FLR をかける(VxWorks は 1)|
| `g_bfm_pcie_mrrs` | Max Read Request Size(0 なら触らない)|
| `g_bfm_dma_base` / `_size` | DMA 領域 |
| `g_bfm_mr_base` / `_size` | 公開メモリ |
| `g_bfm_fw_pages_max` | FW に渡すページの上限 |
| `g_bfm_stage_size` | 接続ごとの中継バッファ |
| `g_bfm_local_ip` / `g_bfm_local_mac` | 自分の IPv4 / MAC(MAC が全部 0 なら NIC の固有の MAC)|
| `g_bfm_peer_ip` / `g_bfm_peer_mac` | 既定の相手 |
| `g_bfm_cm_port` | 接続の番号 |
| `g_bfm_path_mtu` / `g_bfm_port_mtu` | RoCE の経路 MTU(1〜5)/ ポートの MTU |
| `g_bfm_bf_enable` / `g_bfm_scatter_cqe` / `g_bfm_atomic_host_endian` | 機能の入り切り |
| `g_bfm_ack_timeout` / `g_bfm_retry_count` / `g_bfm_op_timeout_ms` | 再送と時間切れ |
| `g_bfm_cpu_awake` | CPU を深い省電力状態に入れない(相手からの read / atomic の遅延に効く。VxWorks では下の 6)|
| `g_bfm_link_wait_ms` / `g_bfm_signal_every` / `g_bfm_verbose` | そのほか |

`BFM_MAX_CONN`(接続の数)は配列の大きさになるのでコンパイル時の定数。

### BSP 側で用意すること

1. DMA 領域と公開メモリの物理メモリを OS に使わせず、論理 = 物理で張る。
   NIC と CPU のキャッシュが一貫しない CPU では、キャッシュしない属性で張るか、
   `BFM_VX_CACHE_OPS` を定義して `cacheFlush` / `cacheInvalidate` を使う。
2. BAR0(`g_bfm_bar0_addr` から BAR0 の大きさぶん)を論理 = 物理・キャッシュしない属性で張る。
3. BlueFlame を使うなら、BAR0 全体を **write-combining** で張った別名を作り、`g_bfm_bar0_wc_va` に入れる
   (x86 は PAT の WC、ARM64 は Normal-NC か Device-GRE)。
4. ECAM が無い機材では `BFM_VX_PCI_LIB` を定義して pciConfigLib を使う。
5. **`bfm_init()` が設定するのは ConnectX の function だけ。** 途中のブリッジ(ルートポートやスイッチ)の
   バス番号・メモリの窓(Memory Base / Limit に BAR0 が入ること)・Memory Space / Bus Master は、
   BSP か呼び出し側で先に設定しておくこと。
6. **受け側の CPU の深い C-state(パッケージの C-state、C1E)を BIOS / BSP で止める。** 相手からの read / atomic は
   受け側の NIC がメモリを読んで応答するので、CPU が深く眠っているとその分待たされる(ConnectX-4 Lx + Core i3-8100 で
   約 300ns。`TEST_PLAN.md`「調査」)。VxWorks の移植層は `g_bfm_cpu_awake` を見ない。

### ビルド

`src/*.c` と `port/vxworks/port_vxworks.c` を DKM などのソースに入れ、`include/` をインクルードパスに足す。
`BFM_PORT_LINUX` は定義しない。手元では `make vxcheck` で、仮のヘッダ(`tools/vxstub/`)を使って
文法だけを確かめられる。

## 試験(Linux + VFIO)

### 準備

- 試験する側(bfmem_test): ConnectX を `vfio-pci` に結びつける(同じ IOMMU グループの装置は全部)。
  root で動かす(sysfs の `resource0` / `resource0_wc` を開くため)。hugepage は要らない。
- 相手(bfm_peer): カーネルの mlx5 のまま。`gcc -O2 -Wall -Wextra -o bfm_peer tools/bfm_peer.c -lrdmacm -libverbs`

### 手順(例: 試験する側 192.168.101.11、相手 192.168.101.21)

```bash
make
# bfmem_test が掛け側、bfm_peer が受け側(相手の近隣表に bfmem の MAC を固定で書く。下の注)
sudo ip neigh replace 192.168.101.11 lladdr <bfmem 側の MAC> dev <IF> nud permanent   # 相手
./bfm_peer -a 192.168.101.21                                     # 相手
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 client 192.168.101.21 <相手のMAC>
kill -QUIT $(pgrep -x bfm_peer)                                  # 相手の hash を出し、client の「模型の hash」と比べる
```

```bash
# bfmem_test が受け側、bfm_peer が掛け側(相手は bfmem の MAC を ARP で引けないので近隣表に書く)
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 server
sudo ip neigh replace 192.168.101.11 lladdr <bfmem 側の MAC> dev <IF> nud permanent   # 相手
./bfm_peer -c 192.168.101.11                                                            # 相手
```

- 相手の Linux には、bfmem 側の IP へ向く経路が要る(`ip route replace 192.168.101.11/32 dev <IF> src 192.168.101.21`)。
  rdma_cm が受けた REQ の宛先を経路で確かめるため。
- **同じカードの 2 ポートの間では試せない。** 宛先の GID か MAC が自分のカードのものだと、
  FW が内部で処理しようとしてワイヤに出ない(vfio_nvme で確認。ConnectX-4 Lx)。別の機械と繋ぐこと。
- `-B` で BlueFlame を止めて比べられる。`-x` で最後に公開メモリの外を読み、相手が断ることを確かめる
  (相手の MKey が範囲を持っていることの確認。接続はエラーになる)。

### client の検査

| | 内容 |
|---|---|
| [1] | 初期の中身を 1〜300 バイト・半端な位置で読む、64KiB まとめて読む |
| [2] | write 1〜220 バイト(inline)と 221〜8,220 バイト(中継バッファ)、毎回読み戻す |
| [3] | write_post 1〜32 バイト x 5,000 + flush の後に全体を読み戻す |
| [4] | FAA +1 x 1,000(元の値の並び)、+2^32、CAS 0→1,000、外れる比較値。相手のメモリの並びを表示 |
| [5] | データ領域を読み戻して模型と比べる |
| [6] | 遅延(write / read 8・64 バイト、FAA、CAS)と write_post の速さ。測った後にもう一度読み戻して比べる |
| 最後 | 模型の hash を出す(相手が切断のときに出す hash と同じであること)|
| [7] | (`-x`)公開メモリの外を読むと相手の NIC が REMOTE_ACCESS で断る |

## 確かめたこと / まだのこと

- **2026-11-07: ConnectX-4 Lx(FW 14.32.1900)の実機で、Linux のカーネルの mlx5 の相手と両方向に通した**
  (OptiPlex の bfmem ↔ Precision の `bfm_peer`。手順・結果・遅延は `TEST_PLAN.md`)。
  - 掛け側として: read / write(inline と中継バッファ)/ write_post / CAS / FAA が全部正しく、相手のメモリの
    hash も一致。BlueFlame で 1 回ずつの操作が約 320〜380ns 縮む(write 8B 中央値 1,784 → 1,403ns)。
  - 受け側として: 相手からの read / write / CAS / FAA を NIC が処理し、**公開メモリの範囲の外は MKey が断る**。
  - 新しく使った欄(vport の MAC、`bf` / `log_bf_reg_size`、atomic の能力とホスト順の設定、QPC の `rae` /
    `atomic_mode`、範囲つきの PA の MKey)は実機で正しいことを確かめた。BAR0 の UAR の 4 ページだけが WC に
    なることも PAT の一覧で確かめた。
- **CQE に入って返る atomic の元の値は、HCA の設定によらずビッグエンディアン**(メモリへ書かせた値は設定どおり)。
  `bfm_cas()` / `bfm_faa()` はこれを直して返す。ConnectX-7 でも同じかは未確認(試験の `[4]` で分かる)。
- **相手が Linux のときは、相手の近隣表に bfmem の MAC を固定で書くこと**(掛け側・受け側どちらでも。
  Linux の ib_cm が REP / REJ を送るときに MAC を近隣表で引き、bfmem は ARP に答えない)。bfmem どうしでは要らない。
- **相手からの read / atomic の遅延は、受け側の CPU の C-state で約 300ns 変わる。** `g_bfm_cpu_awake`(既定 1)で
  Linux では対策した(FAA 2,150 → 1,760〜1,930ns)。hugepage(IOMMU の変換を減らす)は効かなかった。
- まだ: **bfmem どうし**(本番の形)、VxWorks、ConnectX-7。`write_post` は BlueFlame なしのほうが速い
  (11.4 → 13.1 Mops)ので、続けて出すときの扱いは見直しの余地がある。
