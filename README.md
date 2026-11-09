# bfmem

ConnectX(mlx5)の **BlueFlame** を使って、ノード間で相手のメモリへ直接
**write / read / atomic(CAS・FAA)** を打つためのライブラリ。T10-DIF(512 + 8 バイトのブロック)を
付け外し・検査しながら運ぶ読み書きもある(NIC のオフロードあり / なし)。RoCE v2 の RC QP を 1 本張り、
WQE を UAR の BlueFlame の窓へ MMIO で書き込んで送る。相手の CPU は何もしない(相手の NIC が応答する)。

- 対象は **x86-64(Intel)の VxWorks(論理アドレス = 物理アドレス)どうし**。設計と試験は
  **x86-64 Linux + VFIO** で行い、OS に依存する部分は移植層(`port/`)に分けてある。
- **前提を絞って簡単にしてある:** CPU は x86-64 だけ(リトルエンディアン、DMA はキャッシュと一貫)、
  **相手は常に bfmem**(接続の手順は bfmem だけの短いもの。Linux の rdma_cm とは繋がらない)、
  VxWorks の設定空間は ECAM だけ、atomic は常にホストのバイト順。
- `vfio_nvme` の mlx5 ドライバ(ConnectX-4 / 4 Lx の実機で確認済み)から、この用途に要る部分だけを
  取り出して作り直した。**NVMe・NVMe/TCP・NVMe-oF・iSCSI・iSER・TCP/IP のスタックは一切含まない。**
- 試験できるのは今のところ ConnectX-4 系だけ。**ConnectX-7 でも動くように書いてある**(下の「ConnectX-7 への備え」)が、
  実機では確かめていない。

## 構成

| 場所 | 中身 |
|---|---|
| `include/bfm_config.h` | **移植のときに設定する変数を全部ここに集めてある**(実体と既定値は `src/bfm_config.c`)|
| `include/bfm.h` | アプリケーションが使う API |
| `include/bfm_port.h` | 移植層の関数の宣言と、x86-64 のバリア・バイト順の道具 |
| `src/bfm_core.c` | `bfm_init()` / `bfm_shutdown()`(全体の状態機械)/ `bfm_poll()` |
| `src/bfm_pci.c` | PCIe の設定空間の設定(FLR・BAR0・Bus Master・MRRS)|
| `src/mlx5_cmd.c` | コマンドキュー(待たずに出して完了を見る)、FW へのページの受け渡し、EQ |
| `src/mlx5_hca.c` | HCA の立ち上げ、UAR・PD・MKey・CQ、ポート、RoCE のアドレス |
| `src/mlx5_qp.c` | QP(RC と GSI)の作成と状態遷移 |
| `src/bfm_cm.c` | 接続の確立と切断(GSI の上の bfmem だけの REQ / REP / BYE。接続の枠ごとの状態機械)|
| `src/bfm_ops.c` | データ経路(BlueFlame で WQE を送る、完了を拾う)|
| `src/bfm_dif.c` | T10-DIF の読み書き(CPU で行う SW と、NIC の署名オフロードを使う HW)|
| `src/bfm_mem.c` | DMA 領域からの静的な切り出し |
| `port/linux_vfio/port_linux.c` | 移植層: Linux + VFIO(試験環境)|
| `port/vxworks/port_vxworks.c` | 移植層: VxWorks(**未検証**。VxWorks 固有の関数は使わない)|
| `port/x86/bfm_x86_asm.S` | x86-64 の命令の関数(`rdtsc`・`cpuid`・`rdmsr`・`wrmsr`・`pause`)。アセンブラ |
| `port/x86/bfm_x86.c` | TSC の周波数を CPUID / MSR から求め、TSC で時刻と待ちを作る。CPU の周波数を MSR で読んで表示する |
| `test/bfm_test.c` | 試験プログラム(server / client / bench / info)|
| `doc/spec/bfmem_spec.adoc` | **関数仕様書**(全ソースの全関数。表・フロー図・関係図、待たずに進める関数には状態遷移図。図は `doc/spec/images/*.drawio.svg`)。`doc/spec/gen.py` で作る。HTML 版は `docs/spec.html`(図を埋め込んだ 1 ファイル。asciidoctor があれば gen.py が一緒に作る)。**Web で見られる: https://hf355051012-ai.github.io/bfmem/spec.html**(GitHub Pages、master の `docs/`)|

## API

```c
#include "bfm.h"

/* 立ち上げ・接続・切断・停止は待たない(下の「待たない API」)。BFM_INPROGRESS の間は呼び続ける。 */
while ((rc = bfm_init()) == BFM_INPROGRESS) { /* ほかの仕事 */ }   /* PCIe・HCA・RoCE の準備 */
bfm_conn_t *c = NULL;
while ((rc = bfm_accept(&c, 0, 0)) == BFM_INPROGRESS) { }        /* 受け側(ポートは g_bfm_cm_port)*/
bfm_conn_t *c = NULL;
while ((rc = bfm_connect(&c, ip, NULL, 0, 0)) == BFM_INPROGRESS) { }  /* 掛け側(相手の MAC は自動)*/

bfm_write(c, off, src, len);                  /* 相手の公開メモリ + off へ。ACK まで待つ */
bfm_write_post(c, off, src, len);             /* 待たない(inline に収まる長さだけ)*/
bfm_flush(c);                                 /* bfm_write_post() の分を待つ */
bfm_read(c, off, dst, len);
bfm_cas(c, off, expect, desired, &old);       /* 8 バイト、off は 8 の倍数 */
bfm_faa(c, off, add, &old);
bfm_cas32(c, off, expect, desired, &old32);   /* 4 バイト、off は 4 の倍数(拡張 atomic)*/
bfm_faa32(c, off, add, &old32);

bfm_dif_t d = { .op = BFM_DIF_INSERT, .mode = BFM_DIF_SW, .app_tag = 0x1234, .ref_tag = lba };
bfm_dif_write(c, off, src, nblocks, &d, &err); /* T10-DIF(下の「T10-DIF」)。終わるまで待つ */
bfm_dif_read(c, off, dst, nblocks, &d, &err);
bfm_dif_write_start(c, off, src, nblocks, &d); /* 起動だけして戻る */
while (bfm_dif_poll(c, &err) == BFM_DIF_INPROGRESS) { /* ほかの仕事 */ }   /* か bfm_dif_wait(c, &err) */

bfm_poll();                                   /* ときどき呼ぶ(切断要求・FW のページ要求)*/
while (bfm_disconnect(c) == BFM_INPROGRESS) { }
while (bfm_shutdown() == BFM_INPROGRESS) { }
```

- 1 本の接続で、**両方のノードが互いのメモリを触れる**(接続のときに両方向へ公開メモリの情報を渡す)。
- `off` は相手が公開した範囲(`g_bfm_mr_base` から `g_bfm_mr_size`)の先頭からのオフセット。
  範囲の外は手元で断り(`BFM_E_PARAM`)、相手の NIC も MKey の範囲で断る。
- `src` / `dst` はどこにあってもよい(NIC が直接読み書きするのは中継バッファで、CPU が写す)。
- **複数のコアから同時に転送できる(64 コアまで)。** 下の「複数のコア」。

### 待たない API(立ち上げ・接続・切断・停止)

`bfm_init()`・`bfm_connect()`・`bfm_accept()`・`bfm_disconnect()`・`bfm_shutdown()` は、中で回って待たない。
待つところに来ると `BFM_INPROGRESS`(正の値 1)を返すので、**`BFM_INPROGRESS` でなくなるまで同じ関数を定期的に呼び続ける**
(呼ぶたびに先へ進み、呼ばなければ進まない)。間にほかの仕事をしてよい。

| 関数 | 待つところ |
|---|---|
| `bfm_init()` | FLR の後の 100ms と設定空間が読めるまで、FW の初期化(最大 120 秒)、コマンドキューの登録の後の 10ms、FW のコマンド 1 つずつの完了、リンク(10ms おきに PAOS を読む)、`INIT_HCA` の後のページ要求(100ms 来なくなるまで、最大 2 秒)|
| `bfm_connect(&c, …)` | FW のコマンド(CQ・QP を作る・状態を進める)の完了、REP(1 秒ごとに REQ を送り直す。`timeout_ms` で時間切れ)|
| `bfm_accept(&c, …)` | REQ(`timeout_ms`、0 なら待ち続ける)、FW のコマンドの完了、REP を送る GSI の空き |
| `bfm_disconnect(c)` | BYE を送る GSI の空き(100ms まで)、`DESTROY_QP` / `DESTROY_CQ` の完了 |
| `bfm_shutdown()` | 全部の接続を畳む、GSI の送信が出ていく(100ms まで)、出したままのコマンドの完了、`TEARDOWN_HCA` の完了 |

- `bfm_connect()` / `bfm_accept()` は、最初に `*pc = NULL` で呼ぶ(接続の枠を取って `*pc` に入れる)。2 回目からは同じ `pc` で呼ぶ
  (`pc` 以外の引数は見ない)。`BFM_OK` なら `*pc` が使える。失敗なら `*pc` は `NULL` に戻る。
- 途中で失敗したときも、作りかけたもの(QP・CQ、立ち上げなら HCA)を消し終えるまでは `BFM_INPROGRESS` を返し、その後に失敗の理由を返す。
- 受け付けを待つのをやめるとき・確立の途中でやめるときは、`bfm_disconnect(*pc)` を `BFM_OK` まで呼ぶ。
- **FW のコマンドの入口は 1 つ**なので、コマンドを出している途中の接続・切断を呼ばなくなると、ほかの接続の確立も進まない。
  接続・切断は最後まで呼び続けること。FW のページ要求への答え(`MANAGE_PAGES`)も同じ入口を使い、`bfm_poll()` と
  接続・切断の呼び出しの中で進む。
- 各関数の状態と移り方は、関数仕様書の状態遷移図(`bfm_init()`・`mlx5_hca_init_step()`・`conn_step()` など)。
- データ経路(write / read / atomic)は今までどおり、完了(ACK・応答)を待ってから戻る。T10-DIF は起動と完了確認を分けられる(下の「T10-DIF」)。

### 複数のコア

**コアごとに別の接続を作って使う。** 1 つの接続 = RC QP 1 本と、その CQ・UAR(BlueFlame の窓)・中継バッファ・
T10-DIF 用の中継バッファで、データ経路(write / read / atomic / T10-DIF)はその接続の資源しか触らない。
鍵も使わないので、別の接続どうしは互いを待たない。

| 呼んでよいもの | 同時に呼べるか |
|---|---|
| データ経路(`bfm_write()`・`bfm_read()`・atomic・`bfm_dif_*()` など)| 別の接続なら、どのコアから同時に呼んでもよい。**1 つの接続を複数のコアから同時に使わないこと** |
| `bfm_connect()` / `bfm_accept()` / `bfm_disconnect()` / `bfm_poll()` | どのコアから同時に呼んでもよい(中で鍵 `g_bfm_ctl_lock` を取る)。`bfm_disconnect(c)` は、`c` で転送しているコアと同時に呼ばない |
| `bfm_init()` / `bfm_shutdown()` | ほかのコアが bfmem を使っていないときに、1 つのコアから |

- 接続の枠は `BFM_MAX_CONN`(既定 64)個まで。実際に用意する数は `g_bfm_max_conn`(既定 64)で減らせる。
  枠 1 つで DMA 領域を約 230KiB と UAR を 1 つ使う(64 個で約 14MiB)。
- 鍵は接続管理(GSI の QP と、受けた REQ の待ち行列)・接続の枠の取り外し・FW のコマンド・EQ の処理を 1 つずつにする。
  x86-64 の `xchg` で取り、取れなければ `pause` しながら回って待つ(OS の機能は使わない)。持つのは 1 回の呼び出しの間だけ
  (REP や REQ を待つ間は持たない)。**回って待つので、同じ CPU の上で、鍵を持つタスクより優先度の高いタスクから
  接続・切断・`bfm_poll()` を呼ばないこと**(タスクはコアに 1 つずつ置くか、優先度をそろえる)。
- 受け側は、受けた REQ を接続の枠の数まで取っておく(同じ掛け側の送り直しは 1 つにまとめる)。64 本が同時に
  掛かってきても、`bfm_accept()` で受けるたびに古い順に受けられる。受け付けを待つ間は接続の枠を 1 つ使う。
- どの接続も相手の同じ公開メモリを触る。別の接続から同じ場所へ書いたときの順は決まらない
  (atomic はどの接続から打っても不可分。試験 `mt` の [3] で確かめた)。
- 接続は CPU に縛られない。どのコアがどの接続を使うかはアプリケーションが決める
  (`struct bfm_conn` は 64 バイトの境界にそろえ、隣の接続とキャッシュラインを分けてある)。

## 設計

### VFIO で作って VxWorks へ移す

ライブラリの本体(`src/`)は OS の機能を何も使わない。使うのは移植層(`include/bfm_port.h`)の
10 個ほどの関数だけ: 設定空間の読み書き、BAR0 を張る、BAR0 の一部を write-combining で張る、
時刻、待つ、表示。x86 の DMA はキャッシュと一貫しているので、キャッシュの操作は持たない。

VxWorks の移植層も **VxWorks 固有の関数を使わない**(使うのは C の `vprintf` だけ)。
時刻と待ちは CPU の TSC で作る(下の「時刻と CPU の周波数」)。

### 時刻と CPU の周波数(`port/x86/`)

CPU の命令を 1 つずつ包んだ関数をアセンブラで持つ(`bfm_x86_asm.S`: `bfm_x86_rdtsc()`・`bfm_x86_cpuid()`・
`bfm_x86_rdmsr()`・`bfm_x86_wrmsr()`・`bfm_x86_pause()`)。`bfm_x86.c` がそれを使って:

- **TSC の周波数を決める。** `g_bfm_tsc_hz`(0 でなければ)→ CPUID 15h(水晶の周波数 x 比)→
  CPUID 16h(基準の周波数)→ `MSR_PLATFORM_INFO`(基準の倍率 x 100MHz)の順。TSC が invariant
  (CPUID 8000_0007h)でなければ注意を表示する。
- **時刻(ナノ秒)と待ち(空回り + `pause`)を TSC から作る。** VxWorks の `bfm_port_time_ns()` /
  `bfm_port_delay_us()` はこれ。待ちは CPU を他のタスクに譲らない(ライブラリの本体は使わない。待つのは `g_bfm_verbose` のときに周波数を測る 10ms だけ)。
- **CPU の名前・TSC・周波数を表示する**(`g_bfm_verbose` のとき): 基準と最小(`MSR_PLATFORM_INFO`)、
  ターボの最大(`MSR_TURBO_RATIO_LIMIT`)、今の倍率(`IA32_PERF_STATUS`)、実効の周波数
  (10ms 空回りする間の `IA32_APERF` / `IA32_MPERF` の比)、パッケージ C-state の上限(`MSR_PKG_CST_CONFIG_CONTROL`)。

`rdmsr` / `wrmsr` はリング 0 の命令なので、**VxWorks ではカーネル(DKM)のタスクから使う**。存在しない MSR を
読むと #GP で落ちるので、CPUID で機能を確かめ、機種ごとの MSR は Intel の family 6(Nehalem 以降の Core / Xeon)
のときだけ読む。Linux のユーザ空間では `rdmsr` を使えないので、Linux の移植層は `/dev/cpu/N/msr` から読み
(`modprobe msr` が要る)、時刻と待ちは OS のもの(`clock_gettime`・`nanosleep`)のまま使う。
Linux でも `bfm_x86_init()` を呼ぶので、`-v` を付けると同じ表示が出て、TSC を OS の時計で測った値と比べられる。

実機で読んだ値(Linux、`modprobe msr`):

| 機械 | TSC(求めた値)| OS の時計で測った TSC | 基準 / 最小 | ターボの最大 | 実効(空回り中)|
|---|---|---|---|---|---|
| OptiPlex(Core i3-8100)| 3600.000 MHz(CPUID 16h)| 3600.02 MHz | 3600 / 800 MHz | なし | 3600 MHz |
| Precision(Xeon W-2245)| 3900.000 MHz(CPUID 16h)| **3911.99 MHz** | 3900 / 1200 MHz | 4700 MHz | 4500 MHz |

**CPUID 16h と MSR の基準の周波数は 100MHz 単位の名目の値で、実際の TSC とずれることがある**(Precision で
0.3%。Linux も起動のときに 3911.999 MHz と測り直している)。時間切れの判定にしか使わないので差し支えないが、
正確な時刻が要るなら `g_bfm_tsc_hz` に測った値を入れる。どちらの CPU も CPUID 15h の水晶の周波数は 0 だった。

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

既定の大きさは DMA 領域 96MiB(そのうち FW のページが `g_bfm_fw_pages_max` = 16,384 ページ = 64MiB)、
公開メモリ 1GiB。DMA 領域が足りなければ `bfm_init()` が必要な量を表示して失敗する。
公開メモリの大きさは 64bit で扱い、PA モードの MKey 1 つで覆うので、大きくしても NIC の資源は増えない
(VxWorks では、その範囲が OS に使われない RAM であること。Linux では起動のたびに確保して VFIO に登録する)。

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

### 接続(bfmem どうし)

RC QP を繋ぐには、相手の QP 番号・開始 PSN と、公開メモリのアドレス・rkey を交換する必要がある。
IP のスタックを持たないので、交換は **RoCE v2 の UD で相手の QP1(GSI)へ** 送る。相手は常に bfmem
なので、IB CM ではなく 64 バイトの短いメッセージを 3 種類だけ使う。

```
掛け側 bfm_connect():    REQ ->(REP が来るまで 1 秒ごとに送り直す) <- REP
受け側 bfm_accept():     <- REQ  QP を RTS にしてから REP ->
切断   bfm_disconnect(): BYE ->(返事は待たない。相手は bfm_poll() で受けて閉じた扱いにする)

[0..3] 'BFM2'  [4] 種類  [5] 経路 MTU  [6..7] 待ち受けの番号
[8..11] 送り手の接続の id  [12..15] 受け手の接続の id  [16..19] QPN  [20..23] 開始 PSN
[24] 受け付ける read / atomic の数  [26..31] MAC  [32..35] IPv4
[36..39] rkey  [40..47] 公開メモリの先頭  [48..55] 大きさ  [56..59] 宛先の IPv4
                                                                   (数はビッグエンディアン)
```

受け側は REP を送る前に RTS にするので、掛け側は REP を受けて RTS にすればすぐ使える(RTU は要らない)。
REP が落ちたら掛け側が REQ を送り直し、受け側は確立済みの接続への REQ に REP を送り直す。

**ARP を使わない。相手の MAC は自動で取る。** 掛け側は相手の IP を引数(か `g_bfm_peer_ip`)で知る。
相手の MAC を知らなければ(引数が NULL で `g_bfm_peer_mac` が全部 0、または渡した MAC が全部 0)、
REQ を **同報**(宛先の MAC が ff:ff:ff:ff:ff:ff。GRH の宛先 GID は相手の IP のもの)で送り、REP に入った相手の MAC を
RC QP と BYE に使う。同報は同じ L2 の別の bfmem にも届くので、受け手は [56..59] の宛先の IPv4 が
自分のものでないメッセージを捨てる。同報が届かない網(L2 を越える経路)では、相手の MAC を引数か `g_bfm_peer_mac` で渡す。
受け側は相手の MAC を REQ から知る。GID は IP から作る(`::ffff:a.b.c.d`、RoCE v2)。
同報の REQ は ConnectX-4 Lx どうしで受け取られることを確かめた(`TEST_PLAN.md`、2026-11-09)。
ConnectX-7 では確かめていない。

### atomic のバイト順

立ち上げのときに、**8 バイトの atomic をホストのバイト順で扱う設定**にする
(`atomic_req_8B_endianness_mode`、Linux の mlx5_core と同じ)。できない装置では `bfm_init()` が失敗する。
両端とも bfmem なので、相手のメモリの中の atomic の語は常にホスト順(リトルエンディアン)になる。
**CQE に入って返る元の値だけは、この設定によらずビッグエンディアン**なので、`bfm_cas()` / `bfm_faa()` が直して返す。

4 バイトの `bfm_cas32()` / `bfm_faa32()` は mlx5 の拡張 atomic(masked atomic。命令 0x14 / 0x15、ctrl の
opmod = 0x08 で大きさ 4 バイト)で出す。IB の規格には無いので、相手も mlx5 の bfmem であることが前提。
両端の装置が拡張 atomic の 4 バイトに対応していること(atomic の能力の `atomic_ops` の bit2・3 と
`atomic_size_qp` の bit2。ConnectX-4 Lx は 0xf / 0x3c で対応。無ければ `BFM_E_NOTSUP`)。

| | 8 バイト | 4 バイト |
|---|---|---|
| 相手のメモリの語 | ホスト順 | **ホスト順**(8 バイト用の設定が効く)|
| CQE で返る元の値 | ビッグエンディアン | ビッグエンディアン |
| 中継バッファで返る元の値(`g_bfm_scatter_cqe` = 0)| ホスト順 | **ビッグエンディアン** |

どれも関数の中で直してホスト順の数で返す(ConnectX-4 Lx の実機で確かめた。2026-11-07)。

### T10-DIF(`src/bfm_dif.c`)

1 ブロック = データ 512 バイト + 保護情報(PI)8 バイトで、PI はデータの直後に置く(520 バイトおき)。
PI は guard(データの CRC16、多項式 0x8BB7)・app tag・ref tag(ブロックごとに 1 増える。Type 1)で、どれもビッグエンディアン。
`bfm_dif_write()` / `bfm_dif_read()` は、入る側(write は自分のメモリ、read は相手のメモリ)の形のブロックを
出る側の形に変えて運ぶ。

| 操作 | 入る側 → 出る側 | すること |
|---|---|---|
| `BFM_DIF_CHECK` | 520 → 520 | PI を検査し、そのまま渡す |
| `BFM_DIF_CHECK_SWAP` | 520 → 520 | PI を検査し、app tag / ref tag を `new_app_tag` / `new_ref_tag` に置き換える(guard はそのまま)|
| `BFM_DIF_INSERT` | 512 → 520 | PI を作って付ける |
| `BFM_DIF_CHECK_DELETE` | 520 → 512 | PI を検査し、取り除く |

- 検査するもの(guard・app tag・ref tag)は `check` で選ぶ。誤りがあれば `BFM_E_DIF` を返し、
  `bfm_dif_err_t` にブロックの番号・種類・あるべき値・実際の値を入れる。
- app tag の 0xffff を「検査しない」とみなす扱い(escape)はしない。
- 出る側の app tag・ref tag は、入る側と同じ値ならバイトをそのまま写し、違えば新しい値を書く。guard は入る側にあれば写す
  (NIC のオフロードと同じ決め方。SW と HW で結果が同じになる)。

**起動と完了確認**

| 関数 | すること |
|---|---|
| `bfm_dif_write_start()` / `bfm_dif_read_start()` | 起動する。最初の分を出してすぐ戻る。戻り値は起動できたかどうかだけ(前の転送が終わっていなければ `BFM_E_BUSY`)|
| `bfm_dif_poll()` | 待たずに進める(届いた分を検査し、空いた面で次の分を出す)。途中なら `BFM_DIF_INPROGRESS`(= 1)、終わったら結果を 1 回だけ返す |
| `bfm_dif_wait()` | 終わるまで進めて結果を返す(`g_bfm_op_timeout_ms` の間まったく進まなければ `BFM_E_TIMEOUT`)|
| `bfm_dif_write()` / `bfm_dif_read()` | 起動して `bfm_dif_wait()` で待つのと同じ |

- 1 つの接続で同時に動かせる DIF の転送は 1 本。終わるまで `src` / `dst` を変えたり手放したりしない。
- 転送が進むのは `bfm_dif_poll()` / `bfm_dif_wait()` の中だけ(割り込みは使わない)。終わるまでどちらかを呼び続ける。
- 接続ごとに DIF 用の中継バッファ(`g_bfm_dif_buf_size`、既定 128KiB)を持ち、2 面(64KiB = 126 ブロックずつ)に分けて
  交互に使う。片方を NIC が運ぶ間に、もう片方を CPU が用意・検査する。普通の write / read の中継バッファとは別なので、
  DIF の転送の途中でも同じ接続で普通の操作を出してよい。
- HW では面ごとに署名つき MKey と PSV を持ち、SIG_ERR の CQE に入っている MKey で、どちらの面の誤りかを見分ける。

**モード(`mode`)**

| | `BFM_DIF_SW`(オフロードなし)| `BFM_DIF_HW`(オフロードあり)|
|---|---|---|
| 使える装置 | どれでも(**ConnectX-4 はこれだけ**)| 署名オフロード(能力の `sho`)がある装置(ConnectX-5 以降を想定、**ConnectX-7 を想定して書いた**)。`bfm_dif_hw_supported()` が 1 |
| 形を変えるもの | CPU(CRC は 8 バイトずつ表を引く)| NIC(通る途中で検査・付け外し)|
| 手順 | 中継バッファで形を変えて RDMA | UMR で署名つき MKey を張り、SET_PSV × 2 の後、その MKey を手元にして RDMA |
| 誤りがあったとき | write はその回の分を送らない | write でも送ってしまうことがある(誤りは SIG_ERR の CQE で後から分かる)|
| 実機での確認 | ConnectX-4 Lx で確認(2026-11-07)| **未確認**(ConnectX-4 Lx に `sho` が無い)|

- HW の作りは Linux の mlx5_ib の署名 MR(`IB_WR_REG_MR_INTEGRITY`、`set_sig_umr_wr()`・`mlx5_set_bsf()`・`set_psv_wr()`)と
  `rdma_rw_ctx_signature_init()` に合わせた。接続・面ごとに署名つき MKey 1 個と PSV 2 個を `bfm_init()` で作る。
  QP は `fre`(UMR を出せる)を立てて作る。UMR の fence は能力の `umr_fence` に従う。
- どちらのモードも、DIF 用の中継バッファの 1 面に入るブロック数ずつ運ぶ。
- 遅延(ConnectX-4 Lx、OptiPlex → Precision、4KiB = 8 ブロック、中央値): SW の write INSERT 5,555ns・
  write CHECK 5,606ns・read CHECK 5,622ns・read CHECK_DELETE 5,679ns(CRC を 1 バイトずつ引いていたときは約 14,100ns)。
- スループット(同じ、300 ブロックずつ、データ 512 バイト分で数える): SW の write INSERT 1,666MB/s・write CHECK 1,689MB/s・
  read CHECK 1,597MB/s・read CHECK_DELETE 1,609MB/s。出しておく分を 1 つにすると約 1,180MB/s で、2 面を交互に使って約 4 割速い。

### ConnectX-7 への備え(実機では未確認)

| 違い | 対応 |
|---|---|
| 能力(HCA cap)の構造が長い | QUERY / SET_HCA_CAP は 4,096 バイトまるごと読み書きする(Linux と同じ)|
| FW の起動が遅い | 初期化待ちの上限を 120 秒にした |
| FW に渡すページが多い | 上限を変数(`g_bfm_fw_pages_max`)にし、足りなければ要求量を表示して止まる |
| 時刻の形式を実時間だけにできる | RoCE の能力 `qp_ts_format` が 0 でなければ QP の `ts_format` を DEFAULT にする(Linux の `mlx5_get_qp_default_ts()` と同じ)|
| `num_vhca_ports` が 0 でない | SET_ROCE_ADDRESS にポート番号を入れる |
| BlueFlame の窓の大きさ | `log_bf_reg_size` から求める(想定外の値なら 512 として警告)|
| atomic の対応 | atomic の能力から QP の `atomic_mode` を決める(Linux の `get_atomic_mode()` と同じ規則)。断られたら atomic を受けない形で繋ぐ。ホスト順の設定ができなければ立ち上げを断る |
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
| `g_bfm_mr_base` / `_size` | 公開メモリ(既定 1GiB)|
| `g_bfm_fw_pages_max` | FW に渡すページの上限 |
| `g_bfm_stage_size` | 接続ごとの中継バッファ |
| `g_bfm_dif_buf_size` | 接続ごとの T10-DIF 用の中継バッファ(2 面に分けて交互に使う。既定 128KiB)|
| `g_bfm_max_conn` | 用意する接続の枠の数(1〜`BFM_MAX_CONN`、既定 64。コアごとに 1 本使う)|
| `g_bfm_local_ip` / `g_bfm_local_mac` | 自分の IPv4 / MAC(MAC が全部 0 なら NIC の固有の MAC)|
| `g_bfm_peer_ip` / `g_bfm_peer_mac` | 既定の相手(MAC が全部 0 なら自動。REQ を同報で送り、REP から取る)|
| `g_bfm_cm_port` | 接続の番号 |
| `g_bfm_path_mtu` / `g_bfm_port_mtu` | RoCE の経路 MTU(1〜5)/ ポートの MTU |
| `g_bfm_bf_enable` / `g_bfm_scatter_cqe` | 機能の入り切り |
| `g_bfm_ack_timeout` / `g_bfm_retry_count` / `g_bfm_op_timeout_ms` | 再送と時間切れ |
| `g_bfm_cpu_awake` | CPU を深い省電力状態に入れない(相手からの read / atomic の遅延に効く。VxWorks では下の 6)|
| `g_bfm_tsc_hz` | TSC の周波数(Hz)。0 なら CPUID / MSR から求める。求められない CPU では入れる |
| `g_bfm_link_wait_ms` / `g_bfm_signal_every` / `g_bfm_verbose` | そのほか |

`BFM_MAX_CONN`(接続の枠の数の上限。既定 64)は配列の大きさになるのでコンパイル時の定数。

### BSP 側で用意すること

1. DMA 領域と公開メモリの物理メモリを OS に使わせず、論理 = 物理・ライトバックで張る
   (x86 の DMA はキャッシュと一貫しているので、キャッシュの操作は要らない)。
2. BAR0(`g_bfm_bar0_addr` から BAR0 の大きさぶん)を論理 = 物理・キャッシュしない属性で張る。
3. BlueFlame を使うなら、BAR0 全体を **write-combining**(PAT の WC)で張った別名を作り、
   `g_bfm_bar0_wc_va` に入れる。
4. PCIe の設定空間は ECAM(`g_bfm_pcie_ecam_base`)で読み書きする。ECAM の位置を入れること。
5. **`bfm_init()` が設定するのは ConnectX の function だけ。** 途中のブリッジ(ルートポートやスイッチ)の
   バス番号・メモリの窓(Memory Base / Limit に BAR0 が入ること)・Memory Space / Bus Master は、
   BSP か呼び出し側で先に設定しておくこと。
6. **受け側の CPU の深い C-state(パッケージの C-state、C1E)を BIOS / BSP で止める。** 相手からの read / atomic は
   受け側の NIC がメモリを読んで応答するので、CPU が深く眠っているとその分待たされる(ConnectX-4 Lx + Core i3-8100 で
   約 300ns。`TEST_PLAN.md`「調査」)。VxWorks の移植層は `g_bfm_cpu_awake` を見ない。
   `g_bfm_verbose = 1` で立ち上げると、MSR から読んだパッケージ C-state の上限が表示される。
7. **bfmem はカーネル(DKM)のタスクから使う。** 物理アドレスをそのまま触り、MSR を `rdmsr` 命令で読むため。

### ビルド

`src/*.c`・`port/vxworks/port_vxworks.c`・`port/x86/bfm_x86.c`・`port/x86/bfm_x86_asm.S` を DKM のソースに入れ、
`include/` と `port/x86/` をインクルードパスに足す。`BFM_PORT_LINUX` は定義しない。64bit(x86-64)でビルドすること
(それ以外は `#error` で止まる)。アセンブラは GNU as の書き方(AT&T、System V AMD64 の呼び出し規約)。
VxWorks のヘッダは使わないので、手元の `make vxcheck` で gcc の文法の検査とアセンブルができる。

## 試験(Linux + VFIO)

### 準備

- 両方の機械で ConnectX を `vfio-pci` に結びつける(同じ IOMMU グループの装置は全部)。
  root で動かす(sysfs の `resource0` / `resource0_wc` を開くため)。hugepage は要らない。
- 相手も bfmem(`bfmem_test server`)。Linux のカーネルの mlx5 とは繋がらない。

### 手順(例: 掛け側 192.168.101.11、受け側 192.168.101.21)

```bash
make
# 受け側の機械
sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 server
# 掛け側の機械
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 client 192.168.101.21
# 受け側が切断のときに出す hash と、client の「模型の hash」が同じことを見る
# 複数のコア: 16 本のスレッドが別々の接続で同時に転送する(受け側の server はそのまま)
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 -t 16 mt 192.168.101.21
# 相手の MAC は自動で取る(REQ を同報)。同報が届かない網では IP の後ろに相手の MAC を書く
```

`mt` は、スレッドごとに別の接続を同時に張り、次を確かめてから、8 バイトの write / read / FAA を全スレッドで同時に
`-n` 回(既定 100,000)ずつ打って合計の速さを出す。間ずっと別のスレッドが `bfm_poll()` を回し、最後に全スレッドが同時に切断する。
スレッドは CPU に 1 つずつ固定する(CPU の数より多ければ重ねる)。

| 検査 | 内容 |
|---|---|
| [1] | スレッドの区画(相手の公開メモリの 4MiB + 番号 × 64KiB)へ 4KiB を write して read で読み戻す(64 回)|
| [2] | 8 バイトの write と read で読み戻す(1,000 回)|
| [3] | 全スレッドが同じ語へ FAA を `-n` 回ずつ打ち、最後に `-n` × スレッド数になる(接続をまたいだ atomic)|
| [4] | T10-DIF(SW)の write INSERT(`bfm_dif_poll()` で回す)と read CHECK_DELETE(`bfm_dif_wait()`)で元のデータに戻る |

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
| [4] | FAA +1 x 1,000(元の値の並び)、+2^32、CAS 0→1,000、外れる比較値。相手のメモリの語がホスト順であること |
| [4b] | 4 バイトの FAA +1 x 1,000、0 への折り返し、CAS 0→1,000、外れる比較値。相手のメモリのバイト順、隣の語が変わらないこと |
| [5] | データ領域を読み戻して模型と比べる |
| [5b] | 相手の公開メモリの最後の 64KiB と 8 バイトを write して読み戻す。はみ出す操作は手元で断る(`BFM_E_PARAM`)|
| [6] | 遅延(write / read 8・64 バイト、FAA、CAS、FAA32、CAS32)と write_post の速さ。測った後にもう一度読み戻して比べる |
| 最後 | 模型の hash を出す(相手が切断のときに出す hash と同じであること)|
| [8] | T10-DIF: 4 つの操作を write / read の両方(8 通り)で、相手のメモリ・手元の結果と独立に作った期待値を比べる。誤りの検出(guard・ref tag・app tag)。起動と完了確認を分けた使い方(途中の 2 本目の起動が `BFM_E_BUSY`、途中で普通の write / read、`bfm_dif_poll()` で完了、3 つ目の分の誤りを `bfm_dif_wait()` で受け取る)。SW は常に、HW は使えるときだけ。使えなければ `BFM_E_NOTSUP` を確かめる。DIF の遅延とスループット |
| [7] | (`-x`)公開メモリの外を読むと相手の NIC が REMOTE_ACCESS で断る |

## 確かめたこと / まだのこと

- **2026-11-07: bfmem どうし(OptiPlex ↔ Precision、どちらも ConnectX-4 Lx + VFIO)で両方向に通した。**
  bfmem だけの接続の手順(REQ / REP / BYE)に作り直した後の確認で、[1]〜[7] が全部 OK、hash も一致
  (`TEST_PLAN.md` の最後)。
- 作り直す前(IB CM のころ)に、ConnectX-4 Lx(FW 14.32.1900)の実機で、Linux のカーネルの mlx5 の相手
  (当時の `tools/bfm_peer.c`)と両方向に通した。手順・結果・遅延は `TEST_PLAN.md` の段階 0〜5。
  - 掛け側として: read / write(inline と中継バッファ)/ write_post / CAS / FAA が全部正しく、相手のメモリの
    hash も一致。BlueFlame で 1 回ずつの操作が約 320〜380ns 縮む(write 8B 中央値 1,784 → 1,403ns)。
  - 受け側として: 相手からの read / write / CAS / FAA を NIC が処理し、**公開メモリの範囲の外は MKey が断る**。
  - 新しく使った欄(vport の MAC、`bf` / `log_bf_reg_size`、atomic の能力とホスト順の設定、QPC の `rae` /
    `atomic_mode`、範囲つきの PA の MKey)は実機で正しいことを確かめた。BAR0 の UAR の 4 ページだけが WC に
    なることも PAT の一覧で確かめた。
- **CQE に入って返る atomic の元の値は、HCA の設定によらずビッグエンディアン**(メモリへ書かせた値は設定どおり)。
  `bfm_cas()` / `bfm_faa()` はこれを直して返す。ConnectX-7 でも同じかは未確認(試験の `[4]` で分かる)。
- **4 バイトの CAS / FAA(拡張 atomic)を両方向・通常 / `-S` / `-B` / `-x` で通した**(2026-11-07)。
  中継バッファで返る 4 バイトの元の値だけがビッグエンディアンで、最初は `-S` で NG になった。
  遅延は 8 バイトと同じ(中央値 FAA32 1,623ns / CAS32 1,662ns、8 バイトは 1,638 / 1,686ns)。
- **T10-DIF のオフロードなし(SW)を両方向・通常 / `-S` / `-B` で通した**(2026-11-07)。4 つの操作の write / read と
  誤りの検出が全部 OK。起動と完了確認を分けた後も同じ 3 通りで全部 OK。
  **オフロードあり(HW)は、ConnectX-4 Lx に署名オフロード(`sho`)が無いので試せていない。**
- **相手からの read / atomic の遅延は、受け側の CPU の C-state で約 300ns 変わる。** `g_bfm_cpu_awake`(既定 1)で
  Linux では対策した(FAA 2,150 → 1,760〜1,930ns)。hugepage(IOMMU の変換を減らす)は効かなかった。
- **複数のコアから同時に転送できることを確かめた**(2026-11-07)。Precision(16 スレッド)から OptiPlex へ `mt` を
  1・2・4・8・16・32・64 本、OptiPlex(4 スレッド)から Precision へ 1・4・64 本で流し、全部 OK(64 本が同時に接続・切断)。
  8 バイトの write / read の合計は CPU の数までほぼ本数に比例して伸び、16 本で write 10.9 Mops・read 8.5 Mops
  (1 本では 0.76 / 0.61 Mops)。FAA は 8 本以上で約 2.55 Mops で頭打ち(受け側の OptiPlex。原因は確かめていない)。
- まだ: VxWorks、ConnectX-7、T10-DIF のオフロード(`BFM_DIF_HW`)。VxWorks の上で複数のコアから使うこと。`write_post` は BlueFlame なしのほうが速い
  (11.4 → 13.1 Mops)ので、続けて出すときの扱いは見直しの余地がある。
