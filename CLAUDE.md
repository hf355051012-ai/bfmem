## Language Rules
- Respond in Japanese for all interactions, explanations, and code commentary.
- Show all intermediate thinking, progress updates, tool explanations, and final outputs in Japanese.
- Maintain Japanese even when editing code comments or writing commit messages (unless instructed otherwise).

# bfmem 作業メモ

設計・API・移植・試験の手順は `README.md`。この文書は「作業のやり方」と「踏んだ落とし穴」だけを書く。

## 何か

ConnectX(mlx5)の BlueFlame で、ノード間のメモリへ write / read / atomic を打つライブラリ。
`vfio_nvme` の mlx5 ドライバから、この用途に要る部分だけを取り出して作り直した。
**NVMe・NVMe/TCP・NVMe-oF・iSCSI・iSER・TCP/IP のコードはソースに入れない**(ユーザの指示)。
本番は x86-64 の VxWorks(論理 = 物理)どうし。試験は x86-64 Linux + VFIO。

## 守ること

- **移植のときに設定する値は `include/bfm_config.h` に集める**(ユーザの指示)。設定を足すときは、
  宣言をここに、実体と既定値を `src/bfm_config.c` に置き、README の表にも足す。
- **本体(`src/`)は OS の機能を使わない。** OS に依存するものは移植層(`include/bfm_port.h` の関数)へ。
- NIC に渡すアドレス = CPU のポインタの値(VxWorks は論理 = 物理、Linux は仮想 = IOVA に張る)。
  アドレスの変換を本体に持ち込まない。
- ConnectX-7 でも動く前提で書く(README「ConnectX-7 への備え」)。CX-4 だけで通る書き方をしない。
- **前提を絞って簡単にしてある(ユーザの指示)。** 外れる場合のコードを足し戻さない:
  CPU は x86-64 だけ(リトルエンディアン・DMA はキャッシュと一貫・TSO。他の CPU の分岐やキャッシュ操作は持たない)、
  **相手は常に bfmem**(接続は bfmem だけの REQ / REP / BYE。IB CM・Linux の rdma_cm とは繋がらない)、
  VxWorks の設定空間は ECAM だけ、atomic は常にホスト順(できない装置は `bfm_init()` が断る)。
- コードのコメントは日本語。

## ビルド

```bash
export MSYS_NO_PATHCONV=1
wsl.exe -d Ubuntu-22.04 -- bash -c "cd /mnt/c/Users/fukud/Documents/bfmem && make && make vxcheck"
```

## 関数仕様書(`doc/spec/`)

`doc/spec/bfmem_spec.adoc`(AsciiDoc、1 ファイル)と図(`doc/spec/images/*.drawio.svg`、draw.io で編集できる SVG)は
`doc/spec/gen.py` が作る。WSL に asciidoctor(`sudo apt install asciidoctor`)があれば、図を埋め込んだ
`doc/spec/bfmem_spec.html` も一緒に作る(警告が出たら失敗にする)。**ソースの関数を足す・消す・引数を変える・制御の流れを変えたら、`doc/spec/desc/*.json` を直して作り直すこと。**

```bash
wsl.exe -d Ubuntu-22.04 -- bash -c "cd /mnt/c/Users/fukud/Documents/bfmem && python3 doc/spec/gen.py --check && python3 doc/spec/gen.py"
```

- 宣言・引数の型・呼び出しの関係図はソースから自動で取り出す(`--check` が、記述とソースの関数・引数の名前の食い違いを見つける)。
  処理内容・引数の意味・戻り値・前提条件・注意事項・フローは `desc/*.json` に手で書く(書き方は `desc/README.md`)。
- **フローの検査はできない**(書いた流れがソースと合っているかは、人が突き合わせる)。直したらその関数のフローも見直すこと。
- switch の case の終わりの `break` はフローに書かない(書くと外側のループを抜ける意味になる)。
- 関数ポインタ経由の呼び出しは解析で拾えないので、記述の `indirect` / `entry_note` で書く。

## 実機の試験

手順・結果は `TEST_PLAN.md` の最後の「bfmem どうしの試験」(OptiPlex と Precision の両方の ConnectX-4 Lx を
vfio-pci にして bfmem_test どうし)。OptiPlex の 2 つの PF は同じ IOMMU グループなので、**両方**を vfio-pci にする。
Precision は PF0(`0000:b3:00.0`)だけでよい。終わったら mlx5_core に戻す。
ホストへの入り方は `kvcache_cmp/CLAUDE.md` と同じ(`ganesha_pnfs/tools/r.sh`)。

## このプロジェクトで踏んだ落とし穴

次の 4 つ(近隣表・CA GUID・librdmacm・受け側の機械)は、IB CM で Linux の相手(`bfm_peer`)と繋いでいたころのもの。
今の bfmem どうしの手順では起きないが、Linux の RoCE の相手を作るときの参考に残す。

- **Linux の相手には、bfmem の MAC を近隣表に固定で書く**(`ip neigh replace … nud permanent`)。bfmem が掛け側でも要る。
  Linux の ib_cm は REP を送るときに近隣表で MAC を引き、bfmem は ARP に答えないので STALE のまま REP が出ない。
- **CA GUID を MAC だけから作ると、立ち上げ直した後に REJ reason 10(stale connection)で断られ続けた。**
  FW は QPN を同じ順に配り直すので、(GUID, QPN) が前の試みの残りと重なる。`bfm_init()` ごとに乱数を混ぜている。
- **CQE に入って返る atomic の元の値はビッグエンディアン**(HCA のホスト順の設定は、メモリへ書かせる値にしか効かない)。
  最初はホスト順として読んで `[4]` が NG になった。`-S`(scatter to CQE なし)と比べて切り分けた。
- **librdmacm: `rdma_destroy_id()` はその id のイベントを全部 ack してから。** `bfm_peer` が切断のイベントの中で
  destroy して止まり、次の接続要求が `rdma resource show cm_id` に CONNECT のまま溜まった(bfmem から見ると
  REP が来ない / REJ 10)。相手のプロセスが生きているかだけでなく、cm_id の状態も見ること。
- **遅延を比べるときは、受け側の機械をそろえる。** 「Linux どうし」と「Linux → bfmem」で受け側の機械が違い
  (Precision と OptiPlex)、機械の差を bfmem の遅さと読みかけた。比べるのは同じ機械の上で交互に流した値。
- **相手からの read / atomic は受け側の CPU の C-state で約 300ns 変わる**(write は変わらない)。受け側が待ちで
  眠ると、NIC のメモリ読み出しが待たされる。測るときは `g_bfm_cpu_awake` の状態(`-C`)を書き添える。
- **r.sh で相手に常駐させるプロセスは `setsid nohup … < /dev/null &` でも ssh が返らないことがある。**
  呼び出しを `timeout` で包み、残ったプロセスは `pgrep -ax` で確かめる。`timeout` で切った ssh の先のプロセスは
  生き残る(段階 4 の前に止めたつもりの `bfm_peer -c` が残り、後から接続してきた)。

## vfio_nvme から引き継ぐ落とし穴

- **BAR0 は sysfs の `resource0` から張る。** VFIO の fd から張ると PAT が BAR 全体を UC で持ち、
  WC が黙って UC- に格下げされる(BlueFlame が効かないだけで何も壊れないので気付きにくい)。
  WC のページは「その mmap の前 / そのページ / 後」に張り直してから差し込む。
- **同じカードの 2 ポートの間では RoCE がワイヤに出ない**(宛先の GID か MAC が自装置のもの)。別の機械と試す。
- **RST2INIT で rre / rwe / rae を入れると ConnectX-4 の FW は BAD_PARAM。** 権限は RTR2RTS で入れる。
  log_rra_max は INIT2RTR で入れる(RTR2RTS では保持されない)。
- **scatter to CQE(cs_req)の QP に通知なしの read を出すと LOCAL_QP_OP で QP が落ちる。** read / atomic は全部通知を求める。
- **FW のページ要求は、1 回の読み取りの最後の要求にだけ答える**(全部に答えると与えすぎる)。
- **TEARDOWN_HCA の後に FW へコマンドを出さない**(MANAGE_PAGES・DISABLE_HCA が返らないことがある)。
- 番号 0 は有効な番号(CQ など)。作ったかどうかは旗で持つ。
- Linux の rdma_cm の相手は、受けた REQ の宛先 IP を経路で確かめる。相手に bfmem 側の IP への経路が要る。
  掛け側にするときは、bfmem が ARP に答えないので相手の近隣表に MAC を書く。
