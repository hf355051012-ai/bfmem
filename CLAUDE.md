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
本番は VxWorks(論理 = 物理)どうし。試験は x86-64 Linux + VFIO。

## 守ること

- **移植のときに設定する値は `include/bfm_config.h` に集める**(ユーザの指示)。設定を足すときは、
  宣言をここに、実体と既定値を `src/bfm_config.c` に置き、README の表にも足す。
- **本体(`src/`)は OS の機能を使わない。** OS に依存するものは移植層(`include/bfm_port.h` の関数)へ。
- NIC に渡すアドレス = CPU のポインタの値(VxWorks は論理 = 物理、Linux は仮想 = IOVA に張る)。
  アドレスの変換を本体に持ち込まない。
- ConnectX-7 でも動く前提で書く(README「ConnectX-7 への備え」)。CX-4 だけで通る書き方をしない。
- コードのコメントは日本語。

## ビルド

```bash
export MSYS_NO_PATHCONV=1
wsl.exe -d Ubuntu-22.04 -- bash -c "cd /mnt/c/Users/fukud/Documents/bfmem && make && make vxcheck"
```

WSL には librdmacm の開発用ヘッダが無いので `tools/bfm_peer.c` は手元でビルドできない(相手の機械でビルドする)。

## 実機の試験

手順・結果は `TEST_PLAN.md`(OptiPlex の ConnectX-4 Lx を vfio-pci にして bfmem、Precision はカーネルの mlx5 で `bfm_peer`)。
OptiPlex の 2 つの PF は同じ IOMMU グループなので、**両方**を vfio-pci にする。終わったら mlx5_core に戻す。
ホストへの入り方は `kvcache_cmp/CLAUDE.md` と同じ(`ganesha_pnfs/tools/r.sh`)。

## このプロジェクトで踏んだ落とし穴

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
