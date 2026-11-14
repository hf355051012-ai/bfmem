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
| `src/bfm_core.c` | 代表 API `bfm_run()`・ポートの立ち上げの状態機械 `bfm_dev_step()`・`bfm_shutdown()` |
| `src/bfm_pci.c` | PCIe の設定空間の設定(FLR・BAR0・Bus Master・MRRS)|
| `src/mlx5_cmd.c` | コマンドキュー(待たずに出して完了を見る)、FW へのページの受け渡し、EQ |
| `src/mlx5_hca.c` | HCA の立ち上げ、UAR・PD・MKey・CQ、ポート、RoCE のアドレス |
| `src/mlx5_qp.c` | QP(RC と GSI)の作成と状態遷移 |
| `src/bfm_cm.c` | 接続の表と接続ごとの状態機械(GSI の上の bfmem だけの REQ / REP / BYE で確立・張り直し・切断)|
| `src/bfm_ops.c` | データ経路(BlueFlame で WQE を送る、完了を拾う)|
| `src/bfm_dif.c` | T10-DIF の読み書き(CPU で行う SW と、NIC の署名オフロードを使う HW)|
| `src/bfm_mem.c` | DMA 領域からの静的な切り出し |
| `src/bfm_fw.c` | FW の更新(フラッシュの FW を FW 自身に書かせる。MCC / MCDA レジスタ)|
| `port/linux_vfio/port_linux.c` | 移植層: Linux + VFIO(試験環境)|
| `port/vxworks/port_vxworks.c` | 移植層: VxWorks(**未検証**。VxWorks 固有の関数は使わない)|
| `port/x86/bfm_x86_asm.S` | x86-64 の命令の関数(`rdtsc`・`cpuid`・`rdmsr`・`wrmsr`・`pause`)。アセンブラ |
| `port/x86/bfm_x86.c` | TSC の周波数を CPUID / MSR から求め、TSC で時刻と待ちを作る。CPU の周波数を MSR で読んで表示する |
| `test/bfm_test.c` | 試験プログラム(run / serve / bench / info。両方のノードで同じものを動かす)|
| `doc/spec/bfmem_spec.adoc` | **関数仕様書**(全ソースの全関数。表・フロー図・関係図、待たずに進める関数には状態遷移図。図は `doc/spec/images/*.drawio.svg`)。`doc/spec/gen.py` で作る。HTML 版は `docs/spec.html`(図を埋め込んだ 1 ファイル。asciidoctor があれば gen.py が一緒に作る)。**Web で見られる: https://hf355051012-ai.github.io/bfmem/spec.html**(GitHub Pages、master の `docs/`)|

## API

```c
#include "bfm.h"

/* 代表 API。どのコアから何本同時に呼んでもよい。BFM_OK になるまで(なった後も、ときどき)呼び続ける。 */
while (bfm_run() != BFM_OK) { /* ほかの仕事 */ }   /* 初期化・init・connect・転送の準備・メモリの公開 */

bfm_conn_t *c = bfm_conn(port, i);            /* ポート port の接続 i(使えなければ NULL)*/
bfm_write(c, off, src, len);                  /* 相手の公開メモリ + off へ。ACK まで待つ */
bfm_write_post(c, off, src, len);             /* 待たない(inline に収まる長さだけ)*/
bfm_flush(c);                                 /* bfm_write_post() の分を待つ */
bfm_read(c, off, dst, len);
bfm_cas(c, off, expect, desired, &old);       /* 8 バイト、off は 8 の倍数 */
bfm_faa(c, off, add, &old);
bfm_cas32(c, off, expect, desired, &old32);   /* 4 バイト、off は 4 の倍数(拡張 atomic)*/
bfm_faa32(c, off, add, &old32);

/* 多重度つき(下の「多重度」)。完了を待たずに 64 個まで出しておける */
bfm_tag_t tw, tr;
bfm_write_async(c, off, src, len, BFM_ACK, &tw);     /* ACK を確かめる write。tag を返す */
bfm_write_async(c, off, src, len, BFM_NOACK, NULL);  /* ACK を確かめない write(tag なし)*/
bfm_read_async(c, off, dst, len, &tr);               /* read。応答は刈り取ったときに dst へ */
bfm_faa_async(c, off, add, &old, &ta);               /* atomic も同じ(bfm_cas_async・bfm_cas32_async・bfm_faa32_async)。
                                                        元の値は刈り取ったときに old へ */
bfm_test(c, tw);                                     /* 済んだか(待たない)。bfm_wait(c, tw) なら済むまで */
bfm_check(c, &inflight);                             /* ときどき呼ぶ(ACK なしの write の時間切れ・エラー)*/

bfm_dif_t d = { .op = BFM_DIF_INSERT, .mode = BFM_DIF_SW, .app_tag = 0x1234, .ref_tag = lba };
bfm_dif_write(c, off, src, nblocks, &d, &err); /* T10-DIF(下の「T10-DIF」)。終わるまで待つ */
bfm_dif_read(c, off, dst, nblocks, &d, &err);
bfm_dif_write_start(c, off, src, nblocks, &d); /* 起動だけして戻る */
while (bfm_dif_poll(c, &err) == BFM_DIF_INPROGRESS) { /* ほかの仕事 */ }   /* か bfm_dif_wait(c, &err) */

if (bfm_conn_state(port, i) == BFM_ST_DOWN)   /* エラー・相手が切った */
    bfm_reconnect(port, i);                   /* 張り直しを頼む(bfm_run() が進める)*/
while (bfm_shutdown() == BFM_INPROGRESS) { }
```

- **両方のノードで同じものを動かす(双方向)。** ポートごとに、IPv4 の小さいほうのノードが掛け側、大きいほうが受け側に
  自動でなり、接続 (ポート p, 番号 i) どうしが対になる。1 本の接続で、**どちらのノードからも相手のメモリを触れる**
  (接続のときに両方向へ公開メモリの情報を渡し、両側の QP が相手からの write / read / atomic を許す)。
- `off` は相手が公開した範囲(`g_bfm_mr_base` から `g_bfm_mr_size`)の先頭からのオフセット。
  範囲の外は手元で断り(`BFM_E_PARAM`)、相手の NIC も MKey の範囲で断る。
- `src` / `dst` はどこにあってもよい(NIC が直接読み書きするのは中継バッファで、CPU が写す)。
- 接続の数は `g_bfm_port_conn[ポート]`(既定 16。上限 `BFM_MAX_PORT_CONN` = 16)。ポートは `BFM_MAX_PORT` = 2 個まで
  (合計 32 本)。0 のポートは使わない。両方のノードで同じ値にする。

### 多重度(完了を待たずに出す write / read / atomic)

`bfm_write()` / `bfm_read()` は 1 回ずつ ACK・応答を待つ(1 回 約 1.4〜1.7µs = 往復の時間)。`bfm_write_async()` /
`bfm_read_async()` と、CAS / FAA の `bfm_cas_async()` / `bfm_faa_async()` / `bfm_cas32_async()` / `bfm_faa32_async()` は
待たずに戻り、**1 本の接続で、種類を混ぜて最大 64 個(`g_bfm_qdepth`)まで出しておける**。

- 仕掛かり中の 1 個ごとに「枠」を 1 つ使う。枠は送り元 / 着地点の中継バッファ(`g_bfm_async_buf_size`、既定 4KiB)を
  1 つずつ持つので、1 回の長さはその大きさまで(inline に収まる 220 バイトまでの write は WQE に埋め込む)。
- **大きい read を多重度いっぱいに出すときは、出しておく量(接続の数 × 多重度 × 長さ)をノードあたり約 2MB に抑える。**
  両方向に全速で読み合うと、DDIO の無い CPU(試験の OptiPlex の i3-8100、L3 6MB)では着地点が L3 に収まらずメモリの帯域の
  上限に当たり、両方向とも遅くなる(32kB × 64 × 4 本 = 8MB で 5.8 → 4.1〜4.8GB/s。32kB × 16 × 4 本なら線の速さのまま、
  遅延は約 1/5)。仕様書 20 章「結果から読めること」。
- **枠が空いていれば待たずに出す。64 個が全部仕掛かり中なら、ACK・read の応答を刈り取って枠を空けてから出す。**
  空くまで回って待つのは、仕掛かり中の操作を出してから `g_bfm_async_timeout_ms`(既定 100ms)まで。超えたら
  `BFM_E_TIMEOUT` を返し、その接続をエラーにする(`bfm_reconnect()` で張り直す)。
- **ACK を確かめる write(`BFM_ACK`)と read は tag を返す。** `bfm_test(c, tag)` で 1 個ずつ、済んだか(`BFM_OK`)・まだか
  (`BFM_INPROGRESS`)・失敗か(負)を確かめる。`bfm_wait()` は済むまで待つ。read の応答は、完了を刈り取ったときに
  `dst` へ写す(`bfm_test()` が `BFM_OK` を返すまで `dst` を手放さない)。write の `src` は出したときに写すので、戻ったら変えてよい。
- **ACK を確かめない write(`BFM_NOACK`)は tag を返さない。** アプリケーションは、新しい write を出していない間も
  `bfm_check(c, &inflight)` を定期的に呼ぶこと。仕掛かり中の ACK を刈り取り、出してから時間切れを過ぎても ACK の
  無いものがあれば表示して `BFM_E_TIMEOUT`、エラー完了があればそのエラーを返す。`inflight` が 0 なら全部届いた。
- 完了はどの関数で刈り取っても同じ(`bfm_test()`・`bfm_check()`・次の `bfm_*_async()`・普通の `bfm_write()` など)。
  完了は出した順に来るので、ある tag が済めば、それより前に出したものも済んでいる。刈り取った枠はすぐ次の操作に使い、
  使い回された古い tag は `bfm_test()` が `BFM_OK` を返す(失敗した操作があると接続がエラーになって次を出せないので、
  使い回されるのは成功した枠だけ)。張り直す前に出した操作の tag は、張り直した後は `BFM_E_STATE`(結果はもう
  分からない)を返す(結果は張り直す前に確かめること)。
- 全部の WQE に完了通知を求める(ACK を確かめない write も。枠を空ける時と時間切れを知るため)。read / atomic の
  同時の数は QP の上限(相手が受け付ける数、16)を NIC が守る(超えた分は NIC の中で待つ)。

### 再送(フレームが抜けたとき)

接続は RC の QP なので、抜けたフレームは NIC がハードウェアで送り直す(CPU もライブラリも関わらない)。

| 抜けたフレーム | 送り直すまで |
|---|---|
| 途中のフレーム(長い write の途中、または後ろに別の操作が続いている)| 受け側が番号(PSN)の飛びに気付いて NAK を返し、すぐ(往復 1 回で)送り直す |
| 後ろに続くもののない最後のフレーム | ack timeout が過ぎてから送り直す。**ConnectX-4 Lx は `g_bfm_ack_timeout` によらず約 0.37〜0.40 秒待つ**(4〜16 で同じ。0 は送り直さない)|

- 再送が尽きると(既定 7 回。約 3 秒)、NIC は `TRANSPORT_RETRY_EXC` を返す(`BFM_E_RETRY`。接続はエラー)。
- **最後のフレームが抜けたとき、1 回ずつ待つ `bfm_write()` など(時間切れ 5 秒)は再送で救われるが、多重度つきの操作
  (時間切れ `g_bfm_async_timeout_ms` = 100ms)は再送より先に `BFM_E_TIMEOUT` になる**(接続はエラーになり、張り直すと使える)。
  再送で救いたい場合は、`g_bfm_async_timeout_ms` を約 1 秒以上(再送 2 回ぶん以上)にする。
- 実際の間隔は試験 `run -z` の [10] D で測れる(ACK の返らない接続で `bfm_write()` が `BFM_E_RETRY` になるまでの時間)。
  フレームを狙って 1 つだけ落とす試験はしていない(直結では落とせない)。

### 代表 API `bfm_run()`

外から `bfm_run()` を**周期的に呼び続けるだけで、初期化(PCIe)→ init(HCA)→ connect → 転送の準備 →
メモリの公開(相手からのアクセスを受ける準備)まで進む**。中で回って待たない(待つところでは戻る)。

- **接続ごとに状態機械を持つ。** 接続の表(`g_bfm_conn[]`。ポート × 16 行)の行ごとに、どの状態にいるか(`step`)と
  鍵(`lock`)を持つ。`bfm_run()` は表を回り、**鍵を取れた行の状態を 1 つ進める**(取れなければ待たずに次の行へ)。
  最初に鍵を取ったコアが最初の状態を処理し、次の状態も、そのとき鍵を取ったコアが処理する。回り始める行はコアごとにずらす。
- ポートの立ち上げ(PCIe・HCA・公開メモリの MKey・GSI・接続の枠)は、そのポートの行が `K_PORT` の間に、
  その行を進めたコアが進める(ポートの状態機械 `bfm_dev_step()`)。
- 行の状態を進めるときは、行の鍵に加えて全体の鍵 `g_bfm_ctl_lock` も取る(FW のコマンドの入口・GSI・EQ はポートで 1 つ、
  DMA 領域の切り出しは全体で 1 つなので)。こちらも取れなければ待たずに次へ行く。
- **データ経路(write / read / atomic / T10-DIF)は鍵を使わない。** 使えるようになった接続は、そのコアが鍵なしで使う。

| 接続の状態(`bfm_conn_state()`)| 中の手順(`step`)| 待つところ |
|---|---|---|
| `BFM_ST_INIT` | `K_PORT` | FLR の後の 100ms、FW の初期化(最大 120 秒)、コマンド 1 つずつの完了、リンク、`INIT_HCA` の後のページ要求(最大 2 秒)、GSI と接続の枠の用意 |
| `BFM_ST_CONNECT` | `K_WAIT_REQ`(受け側)・`K_CQ`・`K_QP`・`K_INIT`・`K_WAIT_REP`(掛け側)・`K_RTR`・`K_RTS`・`K_REP`(受け側)| REQ、FW のコマンドの完了、REP(`g_bfm_req_retry_ms` = 1 秒ごとに REQ を送り直す。**相手のノードが立ち上がるまで続ける**)|
| `BFM_ST_UP` | `K_UP` | 使える(`K_RTS` で相手からのアクセスを許し、データ経路を準備した後)|
| `BFM_ST_DOWN` | `K_DOWN` | エラー・相手の BYE。`bfm_reconnect()` を待つ(データ経路で使っているかもしれないので、勝手には畳まない)|
| `BFM_ST_FAILED` | `K_FAILED` | ポートの立ち上げの失敗・FW のコマンドの失敗。`bfm_shutdown()` の後にやり直す |

- `bfm_run()` の戻り値は全体の様子: `BFM_OK` = 全部の接続が使える、`BFM_INPROGRESS` = 途中、
  負 = 失敗した接続がある、`BFM_E_STATE` = エラー・切断のままの接続がある。どの値でも呼び続けてよい(ほかの接続は進む)。
- **使えるようになった後も、ときどき呼び続けること。** 相手からの切断の知らせ(BYE)と FW のページ要求に答える。
- `bfm_reconnect(port, i)` は、使える・エラーのままの接続を畳んで(相手へ BYE を送り)、確立からやり直すよう頼む。
  その接続で転送しているコアが無いときに呼ぶ。相手のノードでは、その接続が `BFM_ST_DOWN` になるので、相手も `bfm_reconnect()` を呼ぶ。
- 各状態と移り方は、関数仕様書の状態遷移図(`bfm_cm_conn_step()`・`bfm_dev_step()`・`mlx5_hca_init_step()` など)。
- 接続ごとの状態遷移(中の手順 `K_*` の移り変わり)は、移った時刻・移したコア・とどまった時間をつけてメモリに記録している
  (最後の `BFM_STEPLOG_N` 回。既定 64)。`bfm_conn_steplog(port, i)` で表示する。`bfm_shutdown()` の後も残り、
  次の `bfm_run()` が表を作り直すときに消える。`bfmem_test` は `-L` で、止めた後に全部の接続の記録を表示する。
  コアは移植層の `bfm_port_cpu_id()`(Linux は CPU の番号、VxWorks は CPUID で読んだ APIC ID)。例:

  ```
  bfm: ポート 0 の接続 0 の状態遷移(今 PORT、全 12 回。時刻は接続の表を作った時から、単位は ms)
  bfm:   番号               時刻  コア  遷移                     とどまった時間
  bfm:   0              0.000412     3  IDLE     -> PORT          2913.508120
  bfm:   1           2913.508532     5  PORT     -> CQ               0.021733
  ...
  bfm:   11          9876.543210     0  DESTROY  -> PORT            12.345678(今もとどまっている)
  ```

### 複数のコア

**コアごとに別の接続を使う。** 1 つの接続 = RC QP 1 本と、その CQ・UAR(BlueFlame の窓)・中継バッファ・
T10-DIF 用の中継バッファで、データ経路はその接続の資源しか触らない。鍵も使わないので、別の接続どうしは互いを待たない。

| 呼んでよいもの | 同時に呼べるか |
|---|---|
| データ経路(`bfm_write()`・`bfm_read()`・atomic・`bfm_dif_*()` など)| 別の接続なら、どのコアから同時に呼んでもよい。**1 つの接続を複数のコアから同時に使わないこと** |
| `bfm_run()` / `bfm_conn()` / `bfm_conn_state()` / `bfm_reconnect()` | どのコアから同時に呼んでもよい。`bfm_run()` は鍵を回って待たない(取れた行だけ進める)。`bfm_reconnect()` は、その接続で転送しているコアと同時に呼ばない |
| `bfm_shutdown()` | ほかのコアが bfmem を使っていないときに、1 つのコアから |

- 接続 1 本で DMA 領域を約 230KiB と UAR を 1 つ使う(32 本で約 7MiB)。
- 鍵は x86-64 の `xchg` で取る(OS の機能は使わない)。`bfm_run()` は取れなければ次へ行くので、優先度の違うタスクから
  呼んでも止まらない。回って取るのは `bfm_reconnect()`(行の鍵。持たれるのは状態 1 つを進める間だけ)と
  `bfm_shutdown()`(全体の鍵)だけ。
- 受け側は、受けた REQ をポートの接続の数まで取っておく(同じ掛け側の送り直しは 1 つにまとめる)。
- どの接続も相手の同じ公開メモリを触る(どちらのポートから来ても)。別の接続から同じ場所へ書いたときの順は決まらない
  (atomic はどの接続から打っても不可分。試験 `run` の mt の [3] で確かめる)。
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
| DMA 領域 | `g_bfm_dma_base` / `g_bfm_dma_size` | コマンドキュー・メールボックス・**FW に渡すページ**・EQ / CQ / QP のリング・接続ごとの中継バッファ。ポートの立ち上げのときに静的に切り出す(返さない。全部のポートで共有)|
| 公開メモリ | `g_bfm_mr_base` / `g_bfm_mr_size` | 相手が直接触る範囲。この範囲だけを許した MKey(PA モード)を作る |

既定の大きさは DMA 領域 160MiB(そのうち FW のページがポートごとに `g_bfm_fw_pages_max` = 16,384 ページ = 64MiB)、
公開メモリ 1GiB。DMA 領域が足りなければ立ち上げ(`bfm_run()`)が必要な量を表示して失敗する。公開メモリは全部のポートで同じもので、ポートごとに MKey を作る。
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
掛け側(IP の小さいほう): REQ ->(REP が来るまで 1 秒ごとに送り直す。相手が立ち上がるまで続ける) <- REP
受け側(IP の大きいほう): <- REQ  QP を RTS にしてから REP ->
張り直し・停止:          BYE ->(返事は待たない。相手は bfm_run() の中で受けて BFM_ST_DOWN にする)

[0..3] 'BFM2'  [4] 種類  [5] 経路 MTU  [6..7] 待ち受けの番号
[8..11] 送り手の接続の id  [12..15] 受け手の接続の id  [16..19] QPN  [20..23] 開始 PSN
[24] 受け付ける read / atomic の数  [26..31] MAC  [32..35] IPv4
[36..39] rkey  [40..47] 公開メモリの先頭  [48..55] 大きさ  [56..59] 宛先の IPv4
                                                                   (数はビッグエンディアン)
```

受け側は REP を送る前に RTS にするので、掛け側は REP を受けて RTS にすればすぐ使える(RTU は要らない)。
REP が落ちたら掛け側が REQ を送り直し、受け側は確立済みの接続への REQ に REP を送り直す。
GSI はポートごとにあり、接続 (ポート p, 番号 i) は待ち受けの番号 `g_bfm_cm_port + i` で対になる
(受け側は、その番号宛てで `g_bfm_peer_ip[p]` から来た REQ だけを受ける)。

**ARP を使わない。相手の MAC は自動で取る。** 相手の IP は `g_bfm_peer_ip[ポート]` で知る。
掛け側が相手の MAC を知らなければ(`g_bfm_peer_mac[ポート]` が全部 0)、
REQ を **同報**(宛先の MAC が ff:ff:ff:ff:ff:ff。GRH の宛先 GID は相手の IP のもの)で送り、REP に入った相手の MAC を
RC QP と BYE に使う。同報は同じ L2 の別の bfmem にも届くので、受け手は [56..59] の宛先の IPv4 が
自分のものでないメッセージを捨てる。同報が届かない網(L2 を越える経路)では、相手の MAC を `g_bfm_peer_mac[]` に書く。
受け側は相手の MAC を REQ から知る。GID は IP から作る(`::ffff:a.b.c.d`、RoCE v2)。
同報の REQ は ConnectX-4 Lx どうしで受け取られることを確かめた(`TEST_PLAN.md`、2026-11-09)。
ConnectX-7 では確かめていない。

### atomic のバイト順

立ち上げのときに、**8 バイトの atomic をホストのバイト順で扱う設定**にする
(`atomic_req_8B_endianness_mode`、Linux の mlx5_core と同じ)。できない装置ではポートの立ち上げが失敗する。
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
- SW の CRC16 は、CPU が PCLMULQDQ と SSSE3 を持てば、キャリーなし乗算で 16 バイトずつ畳み込む(Linux の crct10dif-pclmul と
  同じ考え。4 本を並べて進める)。持たなければ 8 バイトずつ表を引く(slice-by-8)。512 バイトあたり、OptiPlex(i3-8100)で
  183ns → 42ns、Precision(Xeon W-2245)で 146ns → 32ns(`bfmem_test crc`。相手も NIC も要らない)。`g_bfm_dif_clmul = 0` で表だけにできる。

**モード(`mode`)**

| | `BFM_DIF_SW`(オフロードなし)| `BFM_DIF_HW`(オフロードあり)|
|---|---|---|
| 使える装置 | どれでも(**ConnectX-4 はこれだけ**)| 署名オフロード(能力の `sho`)がある装置(ConnectX-5 以降を想定、**ConnectX-7 を想定して書いた**)。`bfm_dif_hw_supported()` が 1 |
| 形を変えるもの | CPU(CRC は 8 バイトずつ表を引く)| NIC(通る途中で検査・付け外し)|
| 手順 | 中継バッファで形を変えて RDMA | UMR で署名つき MKey を張り、SET_PSV × 2 の後、その MKey を手元にして RDMA |
| 誤りがあったとき | write はその回の分を送らない | write でも送ってしまうことがある(誤りは SIG_ERR の CQE で後から分かる)|
| 実機での確認 | ConnectX-4 Lx で確認(2026-11-07)| **未確認**(ConnectX-4 Lx に `sho` が無い)|

- HW の作りは Linux の mlx5_ib の署名 MR(`IB_WR_REG_MR_INTEGRITY`、`set_sig_umr_wr()`・`mlx5_set_bsf()`・`set_psv_wr()`)と
  `rdma_rw_ctx_signature_init()` に合わせた。接続・面ごとに署名つき MKey 1 個と PSV 2 個をポートの立ち上げで作る。
  QP は `fre`(UMR を出せる)を立てて作る。UMR の fence は能力の `umr_fence` に従う。
- どちらのモードも、DIF 用の中継バッファの 1 面に入るブロック数ずつ運ぶ。
- 遅延(ConnectX-4 Lx、OptiPlex → Precision、4KiB = 8 ブロック、中央値): SW の write INSERT 5,555ns・
  write CHECK 5,606ns・read CHECK 5,622ns・read CHECK_DELETE 5,679ns(CRC を 1 バイトずつ引いていたときは約 14,100ns)。
- スループット(同じ、300 ブロックずつ、データ 512 バイト分で数える): SW の write INSERT 1,666MB/s・write CHECK 1,689MB/s・
  read CHECK 1,597MB/s・read CHECK_DELETE 1,609MB/s。出しておく分を 1 つにすると約 1,180MB/s で、2 面を交互に使って約 4 割速い。

### FW の更新(`src/bfm_fw.c`。確かめるだけを実機で確認、切り替えは未確認)

`bfm_fw_update(port, img, len, mode)` で、ConnectX のフラッシュの FW を書き換える。`BFM_INPROGRESS` 以外が返るまで
呼び続ける(待たない。`bfm_run()` もその間呼び続けてよい)。方式は Linux の `devlink dev flash` と同じで、
**FW 自身に書かせる**(MCC / MCDA レジスタで FW の中の更新の手順を進める。フラッシュを直接は触らない)。

```c
/* 確かめるだけ: 書いて FW に確かめさせ、切り替えずに取り消す(次の起動の FW は変わらない)*/
while ((rc = bfm_fw_update(0, img, len, BFM_FW_VERIFY_ONLY)) == BFM_INPROGRESS) bfm_run();
/* 切り替える: 確かめた後、次の起動からこの像を使う(電源を入れ直すと有効になる)*/
while ((rc = bfm_fw_update(0, img, len, BFM_FW_ACTIVATE)) == BFM_INPROGRESS) bfm_run();
```

起動しなくなる危険を小さくするためにしていること:

| 危険 | 対策 |
|---|---|
| 書き込みの途中で止まる(電源断・取り消し・誤り)| FW が今動いている像とは別の置き場に書き、確かめてから切り替える。切り替え(ACTIVATE)は FW の確かめ(VERIFY)が通った後にしか出さない。途中で失敗・取り消したら CANCEL と RELEASE を出し、次の起動の像は変えない |
| 別のボード用の像(PSID の違い)| FW が PSID の一致を確かめる装置(MCQI の `match_psid`)でだけ書く。確かめない装置は `g_bfm_fw_allow_no_psid_check` を 1 にしたときだけ |
| 像でないもの・壊れた像 | 先頭の印(`MTFW` の 16 バイト)・像の目次(ITOC)の印と CRC が合わないもの、FW が受けられる大きさを超えるものは書かない。中身は FW が VERIFY で確かめる(ハッシュ・署名が合わなければ断る)|
| ボードごとの情報を消す | 配布の .bin のうち、FW の像の部分(ITOC の相対番地の部分)だけを渡す。ボードごとの情報(MAC・GUID・VPD など。ITOC の device_data の欄)は渡さず、それを指す目次の欄は 0xFF にして渡す(mstflint と同じ)|
| ほかの更新とぶつかる | FW の更新の手順が空き(IDLE)でなければ何もしない(同じカードのもう一方のポート・mstflint などが更新中)|
| 対応していない FW | MCAM で MCQS・MCQI・MCC・MCDA の対応を確かめ、無ければ何もしない |
| リセットでの失敗 | 装置のリセットはかけない。新しい FW は、電源を入れ直したときに有効になる |

- 渡すのは、そのボード(PSID)用の **展開した .bin**(MFA2・zip は断る)。公式の配布元(NVIDIA の
  [ConnectX-4 Lx EN の FW のページ](https://network.nvidia.com/support/firmware/connectx4lxen/)。古い版は Archive Versions)の
  像を使い、配布元の SHA256 で確かめること。
- **配布の .bin はフラッシュ全体の像(ConnectX-4 Lx で 16MB)で、そのまま MCDA で渡すと FW が受けない**(部品の上限は 7MB)。
  mstflint の `FwExtract4MBImage()`(MFA2 を作るときと同じ)にならい、`img_parse()` が ITOC を読んで、先頭から相対番地の
  部分の終わりまで(14.29.1016 で 4,840,404 バイト)を切り出す。先頭の 16 バイト(印)と、ボードの情報を指す ITOC の欄は
  0xFF にして渡す。ConnectX-5 以降の形(FS4)の像は、目次の場所が違うので断る(まだ扱っていない)。
- 同じカードのポートはフラッシュを共有するので、1 つのポートからだけ行う。
- 最初は `BFM_FW_VERIFY_ONLY`(試験の `fw` の既定)で、像が装置に合うことを確かめてから切り替える。
- MCDA で渡すデータは、Linux の `mlx5_reg_mcda_set()` と同じく、4 バイトずつホスト順の値をビッグエンディアンで書く。
  バイト順を誤っても、FW の VERIFY で「ハッシュが合わない」になって取り消すだけで、次の起動の像は変わらない。
- 実機(2026-11-14、Precision の ConnectX-4 Lx、FW 14.29.1016、PSID MT_2420110034)で、同じ版の像を確かめるだけ
  (`BFM_FW_VERIFY_ONLY`)で流して通った(MCDA 1 回 128 バイト、44.6 秒)。mlx5_core に戻した後も running / stored とも
  14.29.1016。**取り消した直後は、FW の立ち上げ(mlx5_core の probe)に約 46 秒かかった**(ふだんは約 1.4 秒)。
- 同じ機械で、最新の 14.32.1912 を `BFM_FW_ACTIVATE` で書いて切り替えた(46.1 秒。次の起動の版 = 14.32.1912)。
- **書いた FW が動き出すのは、カードがリセットされたとき**(ホストの再起動・電源の入れ直し、またはカードだけのリセット)。
  bfmem はリセットをかけない。カードだけのリセットは FW のレジスタ MFRL(0x9028)で頼む。ConnectX-4 Lx(14.29.1016)の
  MFRL は reset_level = 0xc8 で、**level 3(PCI のリセット)に対応している**。MFRL に level 3 を書き、ドライバを外して
  上流のルートポートでリンクを切って繋ぎ直すと、ホストを再起動せずに 14.32.1912 で立ち上がった(手順は `TEST_PLAN.md`)。
  - Linux の `devlink … action fw_activate` は、14.29.1016 では使えない(FW が MCAM に MFRL を載せていない)。14.32.1912 は載せている。
  - bfmem には MFRL とリンクの切断はまだ無い(試験は作業用の版と Linux の `setpci` で行った)。

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

最初の `bfm_run()` の前に代入する。既定値は `src/bfm_config.c`(VxWorks の値は置き場所の例で、
**そのままでは動かない**)。

| 変数 | 意味 |
|---|---|
| `g_bfm_port_conn[ポート]` | **ポートに張る接続の数**(0〜16、既定 16。0 ならそのポートは使わない)。両方のノードで同じ値にする |
| `g_bfm_pci_bus` / `_dev` / `_func[ポート]` | ポートの PCIe の function(ConnectX はポートごとに別の PF)|
| `g_bfm_pcie_ecam_base` | ECAM の先頭(設定空間を直接読み書きする)|
| `g_bfm_bar0_addr[ポート]` | BAR0 に書き込むアドレス(BAR0 の大きさの境界に合わせる。0 なら今の値)|
| `g_bfm_bar0_wc_va[ポート]` | BAR0 全体を WC で張った別名の先頭(0 なら BlueFlame なし)|
| `g_bfm_pcie_flr` | 立ち上げの前に FLR をかける(VxWorks は 1)|
| `g_bfm_pcie_mrrs` | Max Read Request Size(0 なら触らない)|
| `g_bfm_dma_base` / `_size` | DMA 領域(全部のポートで共有。既定 160MiB)|
| `g_bfm_mr_base` / `_size` | 公開メモリ(全部のポートで共有。既定 1GiB)|
| `g_bfm_fw_pages_max` | FW に渡すページの上限(ポートごと)|
| `g_bfm_stage_size` | 接続ごとの中継バッファ |
| `g_bfm_dif_buf_size` | 接続ごとの T10-DIF 用の中継バッファ(2 面に分けて交互に使う。既定 128KiB)|
| `g_bfm_dif_clmul` | DIF の CRC16 に PCLMULQDQ を使う(既定 1 = CPU にあれば使う。0 = 表だけ)|
| `g_bfm_qdepth` | 多重度(1〜`BFM_QD_MAX` = 64、既定 64。完了を待たずに出しておける `bfm_*_async()` の数)|
| `g_bfm_async_buf_size` | 多重度の枠ごとの中継バッファ(既定 4KiB = 非同期の操作の長さの上限。接続 1 本で 64 倍使う)|
| `g_bfm_async_timeout_ms` | 非同期の操作の時間切れ(既定 100ms。枠の空き待ち・`bfm_test()`・`bfm_check()`)|
| `g_bfm_local_ip` / `g_bfm_local_mac[ポート]` | ポートの IPv4 / MAC(MAC が全部 0 なら NIC の固有の MAC)|
| `g_bfm_peer_ip` / `g_bfm_peer_mac[ポート]` | ポートの相手(相手のノードの同じ番号のポート)。**IP の小さいほうが掛け側**。MAC が全部 0 なら自動(REQ を同報で送り、REP から取る)|
| `g_bfm_cm_port` | 接続の番号の起点(接続 i は +i)|
| `g_bfm_req_retry_ms` | 掛け側が REQ を送り直す間隔(既定 1 秒。相手が立ち上がるまで続ける)|
| `g_bfm_path_mtu` / `g_bfm_port_mtu` | RoCE の経路 MTU(1〜5)/ ポートの MTU |
| `g_bfm_bf_enable` / `g_bfm_scatter_cqe` | 機能の入り切り |
| `g_bfm_ack_timeout` / `g_bfm_retry_count` | RC の再送(NIC が行う): ack timeout 4.096µs × 2^N(既定 12)と再送回数(既定 7)。**ConnectX-4 Lx は設定によらず、送り直すまで約 0.37〜0.40 秒待つ**(下の「再送」)|
| `g_bfm_op_timeout_ms` | 1 回ずつ待つ操作の時間切れ(既定 5 秒)|
| `g_bfm_cpu_awake` | CPU を深い省電力状態に入れない(相手からの read / atomic の遅延に効く。VxWorks では下の 6)|
| `g_bfm_tsc_hz` | TSC の周波数(Hz)。0 なら CPUID / MSR から求める。求められない CPU では入れる |
| `g_bfm_fw_allow_no_psid_check` | `bfm_fw_update()`: FW が像の PSID を確かめない装置でも書く(既定 0 = 書かない)|
| `g_bfm_link_wait_ms` / `g_bfm_signal_every` / `g_bfm_verbose` | そのほか |

`BFM_MAX_PORT`(ポートの数の上限。既定 2)と `BFM_MAX_PORT_CONN`(1 ポートの接続の数の上限。既定 16)は
配列の大きさになるのでコンパイル時の定数。接続の表は `BFM_MAX_CONN` = 2 × 16 = 32 行。
`BFM_STEPLOG_N`(接続ごとに覚えておく状態遷移の数。既定 64。1 件 24 バイトで、表全体で 48KiB)も同じ。

### BSP 側で用意すること

1. DMA 領域と公開メモリの物理メモリを OS に使わせず、論理 = 物理・ライトバックで張る
   (x86 の DMA はキャッシュと一貫しているので、キャッシュの操作は要らない)。
2. 使うポートごとに、BAR0(`g_bfm_bar0_addr[]` から BAR0 の大きさぶん)を論理 = 物理・キャッシュしない属性で張る。
3. BlueFlame を使うなら、BAR0 全体を **write-combining**(PAT の WC)で張った別名を作り、
   ポートごとに `g_bfm_bar0_wc_va[]` に入れる。
4. PCIe の設定空間は ECAM(`g_bfm_pcie_ecam_base`)で読み書きする。ECAM の位置を入れること。
5. **bfmem が設定するのは ConnectX の function(使うポートの PF)だけ。** 途中のブリッジ(ルートポートやスイッチ)の
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
- **両方の機械で同じ `bfmem_test run` を動かす**(相手も bfmem。Linux のカーネルの mlx5 とは繋がらない)。
  IP の小さいほうが掛け側になる。どちらを先に立ち上げてもよい(掛け側は相手が立ち上がるまで REQ を送り直す)。

### 手順(例: OptiPlex 192.168.101.11、Precision 192.168.101.21)

```bash
make
# Precision(PF0 だけ)
sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 run 192.168.101.11
# OptiPlex
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 run 192.168.101.21
# 2 ポートとも繋いであれば、ポート 1 の装置・自分の IP・相手の IP も書く(合計 32 本)
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 -D 0000:01:00.0 -I 192.168.102.11 run 192.168.101.21 192.168.102.21
# 両方の「模型の hash」が、相手の「自分の公開メモリの hash」と同じことを見る
```

`run` は次の順に進む(両方のノードが同時に、相手へ同じことをする = 両方向)。

1. `-t` 本(既定 4)のスレッドが `bfm_run()` を同時に呼び続け、全部の接続(`-c` 本 × ポート。既定 16)が使えるまで待つ。
   スレッドごとの呼び出しの回数とかかった時間を出す。
2. 接続 (0, 0) で、相手の公開メモリへ一通りの検査(下の表)。終わったら相手と足並みをそろえる
   (相手の公開メモリの 3MiB の語へ進み具合を書き、自分の同じ語に相手の進み具合が来るのを待つ)。
3. 全部の接続に 1 本ずつスレッドを付け、次を確かめてから、8 バイトの write / read / FAA を `-n` 回(既定 100,000)ずつ、
   64KiB の write / read を `-n` / 50 回ずつ、続けて多重度つき(`bfm_*_async()`。`-Q` 個まで出しておく)の 8 バイトの
   write(ACK なし / あり)・read と FAA・CAS(同じ語へ / 64 個の語へ)を `-n` 回ずつ、4KiB の write / read を `-n` / 10 回ずつ、
   全スレッドで同時に打って合計の速さ(Mops・MB/s)を出す。**区間ごとに相手と
   足並みをそろえる**ので、相手も同じ区間を同時に打つ(= 両方向の速さ)。片方向の速さは、受ける側に `-q` を付けて測る
   (`-q` の側は転送せず、足並みだけそろえる)。本数を変えて速さだけ測るときは、両方に `-P -c <本数>` を付ける
   (`-P` は 2. の一通りの検査を飛ばす)。間ずっと別のスレッドが `bfm_run()` を回す。
   スレッドは CPU に 1 つずつ固定する(CPU の数より多ければ重ねる)。

   | 検査 | 内容 |
   |---|---|
   | [1] | 接続の区画(相手の公開メモリの 4MiB + 番号 × 64KiB)へ 4KiB を write して read で読み戻す(64 回)|
   | [2] | 8 バイトの write と read で読み戻す(1,000 回)|
   | [3] | 全スレッドが同じ語へ FAA を `-n` 回ずつ打ち、最後に `-n` × スレッド数になる(接続・ポートをまたいだ atomic)|
   | [4] | T10-DIF(SW)の write INSERT(`bfm_dif_poll()` で回す)と read CHECK_DELETE(`bfm_dif_wait()`)で元のデータに戻る |

4. (`-x`)最後の接続で公開メモリの外を読み、相手の NIC が断ること、その接続が `BFM_ST_DOWN` になること、
   `bfm_reconnect()` で張り直して読めることを確かめる(相手は BYE を受けて、自分の側を張り直す)。
   続けて、公開メモリの外への ACK を確かめない write の失敗を `bfm_check()` が返すこと、時間切れを 0ms にして
   ACK が返る前に `bfm_check()` すると `BFM_E_TIMEOUT` になる(接続はエラーになる)ことを確かめ、そのたびに張り直す。
5. (`-z`。両方のノードに付ける)**[10] 無応答**。3. の後(`-x` の前)に行う。IP の大きいほう(黙る側)が、最初のポートの
   最後の 3 本の接続の QP を、BYE を送らずに消す(消された QP 宛てのパケットは相手の NIC が黙って捨てるので、ACK も
   応答も返らない)。IP の小さいほう(試す側)はその 3 本で次を確かめ、最後に 3 本とも張り直して読めることを確かめる。

   | | 内容 |
   |---|---|
   | A | ACK を確かめる write で 64 個の枠を全部埋め、65 個目が資源の空きを待って、出してから 100ms で `BFM_E_TIMEOUT`(待った時間も表示)。接続はエラーになり、仕掛かり中の tag も `BFM_E_TIMEOUT` |
   | B | ACK を確かめない write を 10 個出し、`bfm_check()` が 100ms で `BFM_E_TIMEOUT` を報告する(仕掛かり 10 個)|
   | C | read を 1 個出し、`bfm_wait()` が 100ms で `BFM_E_TIMEOUT` を返す |

6. 相手と足並みをそろえてから、模型の hash と自分の公開メモリの hash を出して止める。

- `serve` は接続を張って保つだけ(`bench` の相手。`serve -c 1`)。`info` はポートを立ち上げて能力を表示する。
- `fw` は FW の更新(`bfm_fw_update()`)。ポート 0 だけを立ち上げ(相手は動いていなくてよい)、ファイルの像を書いて
  FW に確かめさせる。**既定は確かめるだけ**(切り替えずに取り消す)で、`-A` を付けたときだけ次の起動から使うよう
  切り替える(電源を入れ直すと有効になる)。Ctrl-C で取り消す。

  ```bash
  sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 fw 192.168.101.11 fw-ConnectX4Lx-xxx.bin       # 確かめるだけ
  sudo build/bfmem_test -A -d 0000:b3:00.0 -i 192.168.101.21 fw 192.168.101.11 fw-ConnectX4Lx-xxx.bin    # 切り替える
  ```
- `perf` は物理コア 1〜4 で、`bfm_write_post`・`bfm_read`・`bfm_read_async`・CAS・FAA・DIF の write / read(CHECK)の
  IOPS・MB/s・平均の遅延を長さごとに測る(両方の機械で動かす = 両方向。`-n` 秒(既定 2)ずつ)。結果は仕様書の 20 章
  (`doc/spec/perf.json`。ログから `python3 doc/spec/perf_import.py OptiPlex=<ログ> Precision=<ログ>` で作り直す)。
- `wpost` は全部のコアの速さと偏りを測る(両方の機械で動かす = 両方向)。CPU に 1 本ずつスレッドを固定し、コアごとに
  接続 1 本(ポートに交互)で 8 バイトの `bfm_write_post()` を `-n` 秒(既定 5)× 3 回打ち、コアごとの Mops・最大/最小・
  変動係数を出す。`-R N` でコアと接続の割り当てをずらせる(遅さがどちらに付くかを見る)。
- `race` は競合の試験(両方の機械で動かす)。`bfm_run()` を休まず呼ぶスレッド `-t` 本を動かしたまま、各ポートの前半の
  接続で転送を確かめ続け、後半の接続で張り直しを繰り返す(`-n` 秒。既定 30。張り直しを回すのは IP の小さいほう)。
  続けて、`bfm_run()` を回したまま `bfm_shutdown()` で止め、回しているスレッドに立ち上げ直させる。

  ```bash
  # Precision / OptiPlex(2 ポート × 16 本)
  sudo build/bfmem_test -t 8 -n 30 -d 0000:b3:00.0 -i 192.168.101.21 -D 0000:b3:00.1 -I 192.168.102.21 race 192.168.101.11 192.168.102.11
  sudo build/bfmem_test -t 8 -n 30 -d 0000:01:00.1 -i 192.168.101.11 -D 0000:01:00.0 -I 192.168.102.11 race 192.168.101.21 192.168.102.21
  ```
- **同じカードの 2 ポートの間では試せない。** 宛先の GID か MAC が自分のカードのものだと、
  FW が内部で処理しようとしてワイヤに出ない(vfio_nvme で確認。ConnectX-4 Lx)。別の機械と繋ぐこと。
- `-B` で BlueFlame を止めて比べられる。

### 一通りの検査(接続 (0, 0))

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
| [9] | 多重度つき: ACK を確かめる write 200 個(64 を超えて出し、刈り取りながら出す)を tag ごとに確かめる、read 200 個を tag ごとに確かめて中身を比べる、使い回された古い tag が `BFM_OK`、write の直後の同じ所の read がその中身を読む、ACK を確かめない write 2,000 個の後に `bfm_check()` で仕掛かりが 0 になり中身が一致、FAA / FAA32 を同じ語へ 200 個出しておくと元の値が 0〜199 の順に並ぶ、CAS / CAS32 の i→i+1 を 200 個出しておくと全部成功する、長すぎる操作・境界の合わない atomic は `BFM_E_PARAM` |
| [8] | T10-DIF: 4 つの操作を write / read の両方(8 通り)で、相手のメモリ・手元の結果と独立に作った期待値を比べる。誤りの検出(guard・ref tag・app tag)。起動と完了確認を分けた使い方(途中の 2 本目の起動が `BFM_E_BUSY`、途中で普通の write / read、`bfm_dif_poll()` で完了、3 つ目の分の誤りを `bfm_dif_wait()` で受け取る)。SW は常に、HW は使えるときだけ。使えなければ `BFM_E_NOTSUP` を確かめる。DIF の遅延とスループット |
| 最後 | 模型の hash を出す(相手が止める前に出す「自分の公開メモリの hash」と同じであること)|

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
- まだ: VxWorks、ConnectX-7、T10-DIF のオフロード(`BFM_DIF_HW`)、FW の更新の後のカードのリセット(MFRL level 3 とリンクの切断。bfmem には無い)。VxWorks の上で複数のコアから使うこと。`write_post` は BlueFlame なしのほうが速い
  (11.4 → 13.1 Mops)ので、続けて出すときの扱いは見直しの余地がある。
- **2026-11-10: 代表 API `bfm_run()`・接続の表(ポート × 16)・2 ポート・両ノード同じプログラム(双方向)に作り直し、
  実機(OptiPlex ↔ Precision、2 ポートとも 25G で直結)で 2 ポート × 16 本 = 32 本を双方向に通した。** 4 本のスレッドから
  `bfm_run()` を呼んで 32 本が張れ(OptiPlex 0.45 秒、Precision 約 2.6 秒)、両方向とも一通りの検査・mt が全部 OK、hash も一致。
  `-x` で切れた接続を `bfm_reconnect()` で張り直せることも確かめた。
  - **帯域は双方向でほぼ倍。** 64KiB の write の合計は、片方向 約 5.0 GB/s → 双方向 9.7 GB/s(77 Gbit/s)、read は
    約 5.1 → 10.1 GB/s(81 Gbit/s)。1 ポートでも双方向の合計は 5.4〜5.8 GB/s(ポートの全二重が効く)。
  - 8 バイトの write / read の Mops は打つ側の CPU で決まり、双方向でも変わらない(OptiPlex 約 2.7 / 2.1、Precision 約 10.5 / 8.3)。
    双方向で落ちたのは、CPU 4 個の OptiPlex が打つ write 64KiB(5.1 → 4.2 GB/s)と FAA(2.2 → 1.7 Mops)。
  - 遅延(中央値)は双方向にすると 30〜70ns 延びる(write 8B 約 1.37µs、read 8B・FAA 約 1.70µs)。
  - **接続の数を変えると**(双方向、`run -P -c <本数>`): 帯域は 1 ポートなら 2 本で頭打ち(合計 5.4〜5.8 GB/s)、2 ポートなら
    各 2 本(合計 4 本)で最も高い(合計 10.6 / 11.2 GB/s)。8 バイトの操作は本数にほぼ比例して伸び(1 本あたり write 約 0.7 Mops)、
    打つ側の CPU の数で頭打ちになる(OptiPlex は 4 本で約 2.6 Mops、Precision は 8 本で 5.5 Mops)。
  - **atomic の多重度つき**: 1 本の接続で、FAA / CAS が多重度 1 の 0.57 Mops から多重度 4 で約 2.1〜2.6 Mops に伸び、そこで頭打ち
    (受ける NIC の atomic の速さ。1 回 約 380〜470ns。同じ語へ打っても 64 個の語へ散らしても同じ)。32 本でも合計は同じところで止まる。
  - **多重度つき(`bfm_*_async()`)**: 1 本の接続・1 つのコアで、8 バイトの write が多重度 1 の 0.75 Mops から 64 で 9.5 Mops、
    read が 0.6 → 7.4 Mops(多重度 8 まではほぼ比例)。4KiB は多重度 4 で 1 ポートの片方向の線速近く(2.7〜2.8 GB/s)。
    2 ポート × 16 本・双方向では 8 バイトの write が合計 24〜47 Mops(1 回ずつ待つ形の 2.6〜10.6 Mops から)。
  - **わざと ACK を返さない試験**(`-z`。相手の QP を BYE なしで消す): 64 個の枠が埋まった後の 65 個目の資源待ち・
    ACK なし write の `bfm_check()`・read の `bfm_wait()` が、どれも出してから 100.0ms で `BFM_E_TIMEOUT` になり、張り直して使えた。
  - 数字の表は `TEST_PLAN.md` の最後。
