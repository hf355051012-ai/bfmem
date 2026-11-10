# 実機試験の段取り(ConnectX-4 Lx、OptiPlex ↔ Precision)

試験の手順の正本はこの文書。結果は各段階の「記録」に書き足す。

**今の手順は最後の「bfmem どうしの試験」。** 2026-11-07 に、相手を常に bfmem とする前提で接続の手順を
bfmem だけのもの(REQ / REP / BYE)に作り直し、Linux の相手(`tools/bfm_peer.c`、librdmacm)は消した。
段階 0〜5 と「調査」は、作り直す前(IB CM で Linux の相手と繋いでいたころ)の記録として残している。

## 前提(2026-11-07 に読んだ状態)

| | OptiPlex(`rpi5-rdma-target`)| Precision(`p5820`)|
|---|---|---|
| カード | ConnectX-4 Lx(0x1015)、カーネル 7.0.0-31 | ConnectX-4 Lx(0x1015)、カーネル 7.0.0-38 |
| PF0 | `0000:01:00.0` `enp1s0f0np0` `b8:3f:d2:1c:eb:80` | `0000:b3:00.0` `ens1f0np0` `b8:ce:f6:73:bf:7e` |
| PF1 | `0000:01:00.1` `enp1s0f1np1` `b8:3f:d2:1c:eb:81` | `0000:b3:00.1` `ens1f1np1` `b8:ce:f6:73:bf:7f` |
| IOMMU グループ | 2 つの PF が**同じグループ 2**(両方を vfio-pci にする必要がある)| PF0 = 2、PF1 = 3(別々)|
| ドライバ | 両方 mlx5_core。IPv4 は無し。リンクは両方 UP | 同左 |
| 動いているもの | ganesha・SPDK・Mooncake・vfio_nvme なし | VM なし、vLLM なし |
| ビルド | `/var/tmp/bfmem` で `make` が警告なしで通る | `/var/tmp/bfm/bfm_peer` が警告なしで通る |

- 配線は vfio_nvme の記録(2026-10-09)では **OptiPlex PF1 ↔ Precision ens1f0np0**、**OptiPlex PF0 ↔ ens1f1np1**。今も同じかは段階 0 で確かめる。
- ConnectX-4 には IPv4 が無いので、ssh は別の NIC を通っている。ConnectX-4 を vfio-pci にしても ssh は切れない。
- IP は試験用: OptiPlex 側 `192.168.101.11`、Precision 側 `192.168.101.21`。どちらも再起動で消える設定だけを使う。
- **同じカードの 2 ポートの間では試さない**(宛先が自分のカードだと RoCE がワイヤに出ない)。

## 段階 0: 配線と相手の道具を Linux どうしで確かめる(両方 mlx5_core のまま)

bfmem を入れる前に、配線と `bfm_peer` が正しいことと、Linux の libibverbs での遅延(比べる基準)を取る。
OptiPlex にも librdmacm のヘッダがあるので、`bfm_peer` を OptiPlex でもビルドして掛け側にする。

```bash
# OptiPlex
sudo ip addr add 192.168.101.11/24 dev enp1s0f1np1
cd /var/tmp/bfmem && gcc -O2 -Wall -Wextra -o bfm_peer tools/bfm_peer.c -lrdmacm -libverbs

# Precision
sudo nmcli device set ens1f0np0 managed no
sudo ip addr add 192.168.101.21/24 dev ens1f0np0
ping -c 3 192.168.101.11
/var/tmp/bfm/bfm_peer -a 192.168.101.21          # 受け側。終わったら pkill -x bfm_peer

# OptiPlex
/var/tmp/bfmem/bfm_peer -c 192.168.101.21 -x
```

- ping が通らなければ、配線が記録と違う。ポートの組を入れ替えて探し、正しい組をこの文書の「前提」に書き直す。
- 合格: `[1]`〜`[4]` が全部 OK。掛け側の「模型の hash」と、受け側が切断のときに出す hash が同じ。
- 遅延(write / read / FAA の平均)を記録する。段階 3 の bfmem の値と比べる基準。

記録(2026-11-07): **合格。**
- 配線は記録どおり OptiPlex PF1(`enp1s0f1np1`)↔ Precision `ens1f0np0`(ping 0.5ms)。
- `[1]`〜`[4]` OK、hash は両側で `4d058e2c177f96e9`。atomic 語 0・1 は相手のメモリでホスト順の 1000。
- Linux(libibverbs、qd=1、1 回ごとに完了を待つ、平均): write 8B 1,776 ns / read 8B 1,846 ns / FAA 1,761 ns。
- **ConnectX-4 Lx の Linux では、atomic の元の値が掛け側へビッグエンディアンで返った**(`atomic_cap` は
  IBV_ATOMIC_HCA = 1)。相手のメモリはホスト順で更新される。`bfm_peer` は最初これをホスト順と決めつけて NG を
  出したので、最初の 0 でない値で並びを決めるように直した。

## 段階 1: OptiPlex の ConnectX-4 を vfio-pci に結びつける

グループ 2 に 2 つの PF が入っているので、**両方**を結びつけ直す(片方だけでは VFIO がグループを開けない)。
OptiPlex の RoCE と Ethernet の netdev(`enp1s0f*`)は、戻すまで消える。

```bash
# OptiPlex
sudo ip addr flush dev enp1s0f1np1
sudo modprobe vfio-pci
for f in 0000:01:00.0 0000:01:00.1; do
  echo vfio-pci | sudo tee /sys/bus/pci/devices/$f/driver_override
  echo $f | sudo tee /sys/bus/pci/devices/$f/driver/unbind
  echo $f | sudo tee /sys/bus/pci/drivers_probe
done
ls -l /dev/vfio/                                    # 2 があること
```

## 段階 2: 立ち上げだけ(`info`)

```bash
# OptiPlex
cd /var/tmp/bfmem
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 -v info
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 -v info      # 2 回目(止めて立ち上げ直せること)
```

見るもの(ここで、mlx5_ifc.h から位置を求めて実機では確かめていない欄を確かめる):

| 表示 | 期待 | 確かめていること |
|---|---|---|
| `ConnectX-4 Lx`、BAR0 32MiB | | PCIe の設定 |
| `MAC b8:3f:d2:1c:eb:81` | PF1 の MAC と同じ | vport の permanent_address の位置 |
| `リンク UP` | | PAOS |
| `bf=1 bf_buf=256` | log_bf_reg_size = 9 の半分 | 能力の `bf` / `log_bf_reg_size` の位置 |
| `ts_format=0` | ConnectX-4 には qp_ts_format が無い | RoCE の能力の位置 |
| `atomic ops=…` / `host_order` / `mode` | ops の下位 2 ビットが立つ | atomic の能力。host_order が 1 なら設定できた |
| `FW ページ` | 約 2,500(vfio_nvme の実測)| ページの受け渡し |
| エラーなしで「止めた」 | | TEARDOWN_HCA |

WC が本当に効いているかは、動かしている間に PAT の一覧で見る
(`sudo grep -i write-combining /sys/kernel/debug/x86/pat_memtype_list` に BAR0 の中のページが出ること)。

記録(2026-11-07): **合格。** 2 回とも同じ結果で、止めて立ち上げ直せた(vfio-pci が開くたびにリセットする)。
- BAR0 0xd8000000(32MiB)、PCIe Gen3 x8、MRRS 512。FW 14.32.1900。
- MAC `b8:3f:d2:1c:eb:81` = PF1 の MAC(vport の permanent_address の位置は正しい)。リンク UP。
- `bf=1`、`log_bf_reg_size` から窓 256 バイト(位置は正しい)。`ts_format=0`。read / atomic は 16 / 16。
- atomic: ops 0xf(標準と拡張の CS / FA)、size_qp 0x3c(4〜32 バイト)、**ホスト順の設定ができた(host_order=1)**、atomic_mode 5(32B)。
- FW のページ: boot 6、init 2,707、INIT_HCA の後の要求 1,238、計 3,951(16,384 の上限に余裕)。DMA 領域は 64.6MiB 使用 / 96MiB。

## 段階 3: bfmem が掛け側、Precision の `bfm_peer` が受け側

```bash
# Precision(rdma_cm が REQ の宛先を経路で確かめるため。ib_cm が REP を送るときに MAC を近隣表で引くため)
sudo ip route replace 192.168.101.11/32 dev ens1f0np0 src 192.168.101.21
sudo ip neigh replace 192.168.101.11 lladdr b8:3f:d2:1c:eb:81 dev ens1f0np0 nud permanent
/var/tmp/bfm/bfm_peer -a 192.168.101.21

# OptiPlex
cd /var/tmp/bfmem
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 client 192.168.101.21 b8:ce:f6:73:bf:7e
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 -B client 192.168.101.21 b8:ce:f6:73:bf:7e   # BlueFlame なし
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 -x client 192.168.101.21 b8:ce:f6:73:bf:7e   # 範囲外
```

- 合格: `[1]`〜`[6]` が全部 OK。最後の「模型の hash」と、`bfm_peer` が切断のときに出す hash が同じ。
  `-x` の `[7]` が OK(相手が REMOTE_ACCESS で断り、こちらがそれを `BFM_E_REMOTE` として返す)。
- `[4]` の「相手のメモリの並び」は Linux 側の設定で決まる(Linux の mlx5_core はホスト順に設定する)ので、
  リトルエンディアンと出るはず。
- `-B` と比べて、BlueFlame ありの write 8B が速いこと(vfio_nvme では約 320ns 縮んだ)。
  段階 0 の Linux の値とも比べる。

記録(2026-11-07): **合格**(通常 / `-S` / `-B` / `-x` の 4 通りとも NG 0、hash は両側で `e92cdeb4e6b0c81d`)。
通るまでに 3 つ直した:
1. **bfmem が掛け側でも、相手の Linux の近隣表に bfmem の MAC を固定で書く必要があった**(上の手順に足した)。
   Linux の ib_cm は REP を送るときに相手の MAC を近隣表で引き、bfmem は ARP に答えないので STALE のまま止まる。
2. **CA GUID に乱数を混ぜた**(`bfm_cm.c`)。MAC だけから作ると、立ち上げ直した後の FW が同じ順に QPN を配るので、
   前の試みの残りと重なって REJ reason 10(stale connection)で断られ続けた。
3. **CQE に入って返る atomic の元の値は、HCA の設定によらずビッグエンディアン**だった(メモリに書かせた値は設定どおり
   ホスト順)。`atomic_result()` を直した。Linux の libibverbs でも同じ(段階 0)。
   ほかに `bfm_peer` の受け側が切断の後に止まる不具合(`rdma_destroy_id()` をイベントの ack より先に呼んでいた)を直した。

遅延(qd=1、中央値 / 平均、ns):

| | bfmem | bfmem `-B`(BlueFlame なし)| bfmem `-S`(CQE なし)| Linux(段階 0、平均)|
|---|---|---|---|---|
| write 8B | 1,403 / 1,439 | 1,784 / 1,831 | 1,420 / 1,458 | 1,776 |
| write 64B | 1,469 / 1,463 | 1,778 / 1,799 | 1,464 / 1,458 | |
| read 8B | 1,707 / 1,722 | 2,031 / 2,048 | 1,814 / 1,829 | 1,846 |
| read 64B | 1,901 / 1,939 | 2,219 / 2,232 | 1,899 / 1,938 | |
| FAA | 1,701 / 1,721 | 2,031 / 2,058 | 1,802 / 1,824 | 1,761 |
| CAS | 1,759 / 1,779 | 2,089 / 2,111 | 1,867 / 1,920 | |
| write_post 8B | 11.40 Mops | **13.12 Mops** | 11.80 Mops | |

- BlueFlame で 1 回ずつの操作は約 320〜380ns 縮む。CQE で応答を受けると read 8B / atomic が約 100ns 縮む。
- **待たずに続けて出す write_post は BlueFlame なしのほうが速い**(WC の窓への 64 バイトの書き込みが CPU を食う。
  rdma-core の mlx5 も BlueFlame は 1 個だけ出すときに限っている)。直すなら write_post だけ 8 バイトのドアベルにする。
- `-x`: 相手(Linux)が範囲外を REMOTE_ACCESS(syndrome 0x13)で断り、bfmem はそれを `BFM_E_REMOTE` で返した。

## 段階 4: Precision の `bfm_peer` が掛け側、bfmem が受け側

bfmem が応答する側(相手から write / read / atomic を受ける)の試験。新しく書いた部分
(公開メモリの範囲つき MKey、QP の `rae` / `atomic_mode`、atomic のホスト順の設定、REP)がここで効く。

```bash
# OptiPlex
cd /var/tmp/bfmem
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 server

# Precision(bfmem は ARP に答えないので、近隣表に MAC を書く)
sudo ip neigh replace 192.168.101.11 lladdr b8:3f:d2:1c:eb:81 dev ens1f0np0 nud permanent
/var/tmp/bfm/bfm_peer -c 192.168.101.11 -x
```

- 合格: `bfm_peer` の `[1]`〜`[4]` が全部 OK。
  - `[3]` の FAA / CAS が OK = こちらの QP が相手からの atomic を受けている(`rae` / `atomic_mode`)。
  - `[4]` が OK = **公開メモリの MKey が範囲の外を断っている**。NG なら PA の MKey が範囲を見ておらず、
    相手が自分のメモリ全体を触れることになる。そのときは MTT の MKey に作り直す(要相談)。
- bfmem の server が切断のときに出す hash が、`bfm_peer` の「模型の hash」と同じ。
- server が出す atomic 語 0・1 が `LE として 1000`(語 0 は 1000、語 1 は 1000)なら、ホスト順の設定が効いている。
  ビッグエンディアンで出たら、atomic の能力の設定が効いていない。

記録(2026-11-07): **合格。**
- `bfm_peer` の `[1]`〜`[4]` が全部 OK。hash は両側で `4d058e2c177f96e9`。
- `[3]` OK = bfmem の QP が相手からの atomic を受けた(`rae` / `atomic_mode` = 5 が効いている)。
- `[4]` OK = **公開メモリの MKey(PA モード、start / len つき)は範囲の外を断る**。bfmem 側には
  WQ_ACCESS_ERROR(0x11)のイベントが来て、接続をエラーにした(相手が範囲外を触った接続を閉じる形で正しい)。
- atomic 語 0・1 は bfmem のメモリで `e8 03 00 …` = ホスト順の 1000(ホスト順の設定が効いている)。
- 遅延(Linux から bfmem へ、平均): write 8B 1,773 / read 8B 2,252 / FAA 2,199 ns
  (Linux どうしでは 1,776 / 1,846 / 1,761。read と atomic で 400ns ほど遅いのは、Linux の掛け側から見た
  bfmem の応答なので、bfmem の受け側の設定のどれか — 経路 MTU、log_rra_max、ack の遅延の名乗り — が効いている
  可能性がある。未調査)。
- 試験の手違い: 段階 4 の前に止めたつもりの `bfm_peer -c` が Precision に残り、ib_cm の REQ の再送で
  bfmem の server に 2 本目の接続ができた。ssh 越しに流すときは、相手のプロセスが止まったことを確かめる。
- WC: bfmem が動いている間の PAT の一覧で、BAR0 の中の 4 ページ(0xd8012000〜0xd8016000、接続の枠 4 つの UAR)
  だけが write-combining、残りは uncached-minus だった。

## 段階 5: 元に戻す

```bash
# OptiPlex
for f in 0000:01:00.0 0000:01:00.1; do
  echo $f | sudo tee /sys/bus/pci/devices/$f/driver/unbind
  echo | sudo tee /sys/bus/pci/devices/$f/driver_override
  echo $f | sudo tee /sys/bus/pci/drivers_probe
done
ip -br link | grep enp1s0f                          # mlx5_core に戻って netdev があること

# Precision
pkill -x bfm_peer
sudo ip neigh del 192.168.101.11 dev ens1f0np0
sudo ip route del 192.168.101.11/32 dev ens1f0np0
sudo ip addr flush dev ens1f0np0
sudo nmcli device set ens1f0np0 managed yes
```

記録(2026-11-07): 戻した。bfmem_test は SIGINT で止まり(TEARDOWN_HCA まで通った)、OptiPlex の 2 つの PF は
mlx5_core に戻って netdev が出た。Precision の IP・経路・近隣表を消し、NetworkManager の管理に戻した。

## うまくいかないとき見るもの

- `-v` を付けて、どのコマンドで止まったか(`status` と `syndrome`)を見る。
- OptiPlex の `sudo dmesg | grep -iE 'dmar|vfio'`(NIC が IOVA の外へ DMA した跡)。
- Precision の `ethtool -S ens1f0np0 | grep -E 'vport_rdma'`(RoCE のパケットが届いているか)。
- 接続で REP が来ない: Precision の経路(段階 3)、`bfm_peer` が動いているか、`rdma_cm` の REJ の理由。
- OptiPlex の NIC が応答しなくなったら、bfmem_test を止める(vfio-pci が閉じるときにリセットする)。
  それでも戻らなければ段階 5 で mlx5_core に戻す。最後の手段は OptiPlex の再起動。

## 今回やらないこと

- **bfmem どうし**(本番の形)。Precision の `0000:b3:00.0`(グループ 2 に単独)も vfio-pci にすれば試せる。
  Precision は VM と GPU のホストなので、やるときは別に了承を取る。
- ConnectX-7(機材が無い)、VxWorks(機材が無い)。

## 調査: 相手からの read / atomic が遅い(2026-11-07)

段階 4 で、Linux から bfmem への read / FAA(平均 2,252 / 2,199ns)が、Linux どうし(段階 0、1,846 / 1,761ns)より
約 400ns 遅かった。write は同じ(1,773 / 1,776ns)。

**原因: 受け側の CPU の深い C-state。** 受け側の NIC は read / atomic の応答のためにホストのメモリを読む。
受け側の CPU が眠っている(パッケージの C-state)と、メモリ側が起きるまでその読み出しが待たされる。write は
相手のメモリを読まないので効かない。bfmem のせいではなかった(下の 1)。

切り分け(Precision の `bfm_peer -c` から OptiPlex へ、20,000 回の平均、ns。交互に流した):

1. **Linux と bfmem を同じ機械(OptiPlex)で受け側にして比べる。** 段階 0 の比較は受け側が Precision(Xeon)、
   段階 4 は OptiPlex(Core i3-8100)で、機械が違っていた。同じ機械なら差は無い(回ごとに ±100ns ばらつく)。

   | 受け側 | read 8B | FAA |
   |---|---|---|
   | Linux(mlx5_core)| 2,184 / 2,317 / 2,143 | 2,138 / 2,161 / 2,121 |
   | bfmem | 2,130 / 2,161(段階 4 は 2,252)| 2,105 / 2,127(段階 4 は 2,199)|

2. **公開メモリを 2MiB の hugepage に載せる**(IOMMU の変換を減らす)→ **効かない**。
   4KiB: read 2,140 / 2,162 / 2,153、hugepage(56 枚使用を確認): 2,269 / 2,258 / 2,175。作業域が小さく、変換は効いていない。

3. **受け側で深い C-state を禁じる**(`/dev/cpu_dma_latency` に 0 を書いて保持)→ **効く**。

   | | read 8B | FAA |
   |---|---|---|
   | 禁じない | 2,297 / 2,165 / 2,165 | 2,158 / 2,132 / 2,133 |
   | 禁じる | 1,943 / 1,819 / 2,053 | 1,809 / 1,777 / 1,845 |

**対策:** `g_bfm_cpu_awake`(既定 1)を足した。Linux の移植層は `bfm_init()` から `bfm_shutdown()` まで
`/dev/cpu_dma_latency` に 0 を書いて保持する。VxWorks は BIOS / BSP で深い C-state を止める(README)。
対策を入れた bfmem で、既定と `-C`(対策なし)を交互に流した:

| bfmem の受け側 | read 8B | FAA |
|---|---|---|
| `-C`(対策なし)| 2,136 / 2,310 / 2,278 | 2,118 / 2,152 / 2,161 |
| 既定(対策あり)| 1,788 / 2,043 / 1,863 | 1,761 / 1,836 / 1,929 |

FAA は Linux どうし(受け側 Precision)の 1,761ns と同じところまで縮んだ。掛け側の bfmem は CQ を回り続けるので
眠らず、掛け側の遅延(段階 3)には効かない。終わった後、OptiPlex は mlx5_core に戻し、hugepage は 0 に戻した。

## 不具合の修正の後の流し直し(2026-11-07)

仕様書を書く途中で見つかった不具合(エラー完了の戻り値、`bfm_cm_init()` の失敗の後始末、`bfm_ops_post_rw()` の範囲の検査、
`bfm_peer` の `-a 0.0.0.0` と後片付け、など)を直した後、段階 2〜4 と性能の測定を流し直した。**全部合格。**
- 段階 2: 立ち上げと停止。表示は前と同じ(FW ページ 3,951、atomic_mode 5)。
- 段階 3: 通常 / `-B` / `-x` で NG 0、hash は `e92cdeb4e6b0c81d`。遅延は前と同じ(write 8B 中央値 1,341ns、read 8B 1,708ns)。
  `bfm_peer` は `-a` なし(0.0.0.0)で待ち、3 本の接続を続けて受けた。
- 段階 4: `bfm_peer -c -x` の `[1]`〜`[4]` が OK、hash は両側で `4d058e2c177f96e9`。
  `bfm_peer -n 0` は断り(戻り値 2)、server が居ないときは UNREACHABLE を表示して止まらずに終わった。
- 性能(各 10,000 回、MTU 1500): 80 通り全部が流れ、値は BENCH.md と同じ(write / read とも 8KiB 以上で約 2.89 GB/s)。
- 終わった後、OptiPlex は mlx5_core に戻した。

## bfmem どうしの試験(今の手順)

相手は常に bfmem なので、**両方の機械の ConnectX を vfio-pci にして bfmem_test を動かす**。
Precision の PF0(`0000:b3:00.0`)は単独の IOMMU グループ(2)なので、それだけを vfio-pci にすればよい
(ssh は別の NIC `eno1` を通るので切れない)。近隣表・経路の設定は要らない(どちらも ARP を使わない)。
相手の MAC は省ける(REQ を同報で送り、REP から取る)。下の手順の MAC は、渡す形の例として残している。

```bash
# 両方: ビルドして vfio-pci にする(OptiPlex は 01:00.0 と 01:00.1、Precision は b3:00.0)
make
echo vfio-pci | sudo tee /sys/bus/pci/devices/<BDF>/driver_override
echo <BDF> | sudo tee /sys/bus/pci/devices/<BDF>/driver/unbind
echo <BDF> | sudo tee /sys/bus/pci/drivers_probe

# 両方で同じ run を動かす(2026-11-10 から。IP の小さい OptiPlex が掛け側になる。どちらを先に立ち上げてもよい)
# Precision
sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 run 192.168.101.11
# OptiPlex。"" / -B / -x / -S の通り(-x は片方だけでも両方でもよい)
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 run 192.168.101.21
# 両方の「模型の hash」が、相手の「自分の公開メモリの hash」と同じことを見る

# bench(Precision で測る。相手は接続 1 本で張って保つ)
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 -c 1 serve 192.168.101.21
sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 -n 10000 bench 192.168.101.11
```

終わったら driver_override を空にして unbind / drivers_probe で mlx5_core に戻す(段階 5 と同じ)。

- 合格(2026-11-10 からの run): 両方のノードで「全部の接続が使える」、一通りの検査 `[1]`〜`[8]`(`-x` では `[7]` も)と
  mt の `[1]`〜`[4]` が全部 OK、両方の向きで「模型の hash」と相手の「自分の公開メモリの hash」が同じ。**まだ流していない。**
- 以下の 2026-11-09 までの記録は、作り直す前の API(server / client / mt)のもの。そのころの合格の条件:
- 合格: client の `[1]`〜`[6]`(`-x` では `[7]` も)が全部 OK。client の「模型の hash」と、server が切断のときに
  出す hash が同じ。server が `相手が接続 0 を切った` を出す(BYE が届いている)。

記録(2026-11-07、作り直した直後): **合格。**
- Precision でも bfmem が立ち上がった(FW 14.29.1016、FW ページ 7,841、atomic_mode 5)。
- OptiPlex → Precision: 通常 / `-B` / `-x` の 3 通りとも NG 0、hash は両側で `e92cdeb4e6b0c81d`。
  server は 3 回とも BYE を受けて閉じた(`-x` の回は先に範囲外の読みで WQ_ACCESS_ERROR になった)。
  遅延(中央値): write 8B 1,335ns / read 8B 1,644ns / FAA 1,638ns(BlueFlame なしでは 1,702 / 1,961 / 1,952ns)。
- Precision → OptiPlex: 通常 / `-x` とも NG 0、hash 一致。bench(各 1 万回)は 8KiB 以上で約 2.89 GB/s と前と同じ。
- 終わった後、両方とも mlx5_core に戻した(Precision の `ens1f0np0` には 192.168.101.21 を付け直した)。

記録(2026-11-07、4 バイトの CAS / FAA を足した後): **合格。**
- client の `[4b]`(`bfm_faa32()` / `bfm_cas32()`)を、OptiPlex → Precision で通常 / `-S` / `-B` / `-x`、
  Precision → OptiPlex で通常 / `-S` の計 6 通り流して全部 OK、hash は両側で `e92cdeb4e6b0c81d`。
- 相手のメモリの語 2 は `04 03 02 01 0d 0c 0b 0a` = ホスト順(8 バイト用のホスト順の設定が 4 バイトにも効く)。
- **最初は `-S` だけ `[4b]` が NG。** 中継バッファへ返る 4 バイトの元の値が `00 00 00 01`(ビッグエンディアン)
  だった(8 バイトはホスト順で返る)。CQE で返る値はどちらもビッグエンディアン。`atomic32_result()` を直した。
- 遅延(中央値、OptiPlex → Precision): FAA32 1,623ns / CAS32 1,662ns(同じ回の 8 バイトは 1,638 / 1,686ns)。
  `-S` では 1,779 / 1,853ns、`-B` では 1,976 / 2,035ns。
- 終わった後、両方とも mlx5_core に戻した(Precision の `ens1f0np0` には 192.168.101.21 を付け直した)。

記録(2026-11-07、T10-DIF を足した後): **オフロードなし(SW)は合格。オフロードあり(HW)は試せない。**
- `info -v`: 両方の ConnectX-4 Lx とも `sho=0`(署名オフロードが無い)、`umr_fence=0`。`bfm_dif_hw_supported()` は 0。
- client の `[8]`(300 ブロック = 中継バッファ 3 回分)を、OptiPlex → Precision で通常 / `-S`、Precision → OptiPlex で
  `-B` の 3 通り流して全部 OK、hash も一致。
  - write INSERT / CHECK / CHECK_SWAP / CHECK_DELETE と read CHECK / CHECK_SWAP / CHECK_DELETE / INSERT の 8 通りが、
    試験の側でビットごとの CRC で作った期待値と一致(相手のメモリは `bfm_read()` で読んで比べた)。
  - 誤りの検出: write のブロック 200 の guard(あるべき 0x09be / 実際 0xc063)、期待する ref tag のずれ(ブロック 0)、
    相手のメモリのブロック 150 の app tag(0x1234 / 0x1274)を、ブロックと種類まで正しく返した。
  - `BFM_DIF_HW` は `BFM_E_NOTSUP` を返した。
- 遅延(4KiB = 8 ブロック、中央値、OptiPlex → Precision): write INSERT 5,470ns / write CHECK 5,573ns /
  read CHECK 5,704ns / read CHECK_DELETE 5,482ns。CRC を 1 バイトずつ引いていた最初の版は約 14,100ns で、
  8 バイトずつ引く形(slice-by-8)にして縮んだ。
- 終わった後、両方とも mlx5_core に戻した(Precision の `ens1f0np0` には 192.168.101.21 を付け直した)。

記録(2026-11-07、T10-DIF の起動と完了確認を分けた後): **オフロードなし(SW)は合格。オフロードあり(HW)は試せない(`sho=0`)。**
- 起動(`bfm_dif_write_start()` / `bfm_dif_read_start()`)と完了確認(`bfm_dif_poll()` / `bfm_dif_wait()`)を分け、
  DIF 用の中継バッファ(2 面)を交互に使う形にした。
- client の `[8]` を、OptiPlex → Precision で通常 / `-S`、Precision → OptiPlex で `-B` の 3 通り流して全部 OK、hash も一致
  (e92cdeb4e6b0c81d)。前からある 4 つの操作 × write / read と誤りの検出に加えて、次を確かめた。
  - write INSERT を起動したまま、2 本目の起動が `BFM_E_BUSY`(-11)、同じ接続の普通の write / read(64 バイト)が使える、
    `bfm_dif_poll()` で最後まで進む(途中の `BFM_DIF_INPROGRESS` は 1,216 回)、動いていなければ `bfm_dif_poll()` は `BFM_OK`。
  - 相手のブロック 280(3 つ目の分 = 面 0 の 2 回目)の app tag を壊した read CHECK で、`bfm_dif_wait()` が
    ブロック 280・app tag を返し、ブロック 0〜279 は dst に届いていた。
- 遅延(4KiB、中央値、OptiPlex → Precision): write INSERT 5,555ns / write CHECK 5,606ns / read CHECK 5,622ns /
  read CHECK_DELETE 5,679ns(分ける前とほぼ同じ)。
- スループット(300 ブロックずつ 2,500 回、データ 512 バイト分で数える):
  - OptiPlex → Precision: write INSERT 1,666MB/s / write CHECK 1,689MB/s / read CHECK 1,597MB/s / read CHECK_DELETE 1,609MB/s
    (`-S` でも同じ)。
  - Precision → OptiPlex(`-B`): 1,831 / 1,812 / 1,821 / 1,834MB/s。
  - 比べるため、OptiPlex の作業用のコピーだけ「出しておく分を 1 つ」に変えて測ると 1,188 / 1,182 / 1,158 / 1,162MB/s。
    2 面を交互に使うと約 4 割速い(CRC の計算と転送が重なる)。
- 終わった後、両方とも mlx5_core に戻した(Precision の `ens1f0np0` には 192.168.101.21 を付け直した)。

記録(2026-11-07、複数のコアから同時に転送できるようにした後): **合格。**
- 接続の枠を 64 にし(`BFM_MAX_CONN`・`g_bfm_max_conn`)、接続管理・FW のコマンド・EQ の処理に鍵を入れ、受けた REQ を
  枠の数まで取っておくようにした。データ経路は接続ごとの資源だけを触る(鍵なし)。DMA 領域は 96MiB のうち 80,740KiB を使う
  (FW に渡すページ 64MiB を含む)。
- 前からある client の試験: OptiPlex → Precision で通常 / `-S`、Precision → OptiPlex で `-B` の 3 通りとも NG 0 件、
  hash 一致(e92cdeb4e6b0c81d)。遅延も前と同じ(write 8B 中央値 1,338ns、read 8B 1,648ns)。
- `mt`(server は相手の機械で 1 本。-n 100000):

  | 掛け側 → 受け側 | スレッド | write 8B 合計 | read 8B 合計 | FAA 合計 | 結果 |
  |---|---:|---:|---:|---:|---|
  | Precision → OptiPlex | 1 | 0.76 Mops | 0.61 Mops | 0.61 Mops | OK |
  | | 2 | 1.44 | 1.16 | 1.16 | OK |
  | | 4 | 2.87 | 2.29 | 2.28 | OK |
  | | 8 | 5.69 | 4.52 | 2.56 | OK |
  | | 16 | 10.90 | 8.48 | 2.55 | OK |
  | | 32(CPU 16 個に重ねる)| 10.90 | 8.60 | 2.55 | OK |
  | | 64(同上)| 10.83 | 8.59 | 2.55 | OK |
  | OptiPlex → Precision | 1 | 0.76 | 0.61 | 0.61 | OK |
  | | 4 | 2.84 | 2.26 | 2.25 | OK |
  | | 64(CPU 4 個に重ねる)| 2.87 | 2.27 | 2.28 | OK |

  - どの回も、全スレッドが同時に接続でき、[1][2][4] の NG は 0、[3] の共有の語への FAA の合計は `-n` × スレッド数に一致した。
  - write / read は CPU の数まで本数にほぼ比例して伸びる(1 本あたり 0.7 / 0.55 Mops 前後のまま)。
  - FAA(スレッドごとに別の語)は、受け側が OptiPlex のとき 8 本以上で約 2.55 Mops で頭打ち。受け側の NIC の atomic の
    処理の上限とみているが、確かめていない。
- 最初は 2 本・8 本の回だけ接続できなかった。原因は試験の段取りで、前の回の server が止まりきる前(止めるのに約 2.1 秒)に
  次の server を立ち上げ、VFIO のグループが空いていなくて「ポート層の初期化に失敗」していた。前の server が消えるまで
  待つようにして解消した(ライブラリの問題ではない)。
- 終わった後、両方とも mlx5_core に戻した(Precision の `ens1f0np0` には 192.168.101.21 を付け直した)。

記録(2026-11-08、公開メモリの既定を 16MiB から 1GiB にした後): **合格。**
- 両方とも公開メモリ 0x110000000 + 0x40000000(1GiB)で立ち上がった(MKey は PA モード 1 つ)。2MiB の hugepage が
  どちらにも無いので、4KiB のページで張っている。DMA 領域の使用量は前と同じ(80,740KiB / 96MiB)。
- client の試験に [5b] を足した: 相手の公開メモリの最後の 64KiB と 8 バイトを write して読み戻す、
  はみ出す操作(最後の 4 バイトから 8 バイトの read、最後の次の 1 バイトの write)は手元で `BFM_E_PARAM`。
- OptiPlex → Precision(`-x`): NG 0 件。[5b] OK、[7] 公開メモリのすぐ外(1GiB の位置)の read を相手の NIC が
  REMOTE_ACCESS で断った(相手の MKey がちょうど 1GiB を覆う)。遅延は前と同じ(write 8B 中央値 1,338ns、read 8B 1,649ns)。
- Precision → OptiPlex: NG 0 件、受け側の hash 一致(e92cdeb4e6b0c81d)。
- Precision の BAR0 は Linux が 0x3800c0000000 に割り当てていた(4GiB より上の 64bit の BAR0 でも動く)。
- 終わった後、両方とも mlx5_core に戻した(Precision の `ens1f0np0` には 192.168.101.21 を付け直した)。

記録(2026-11-09、相手の MAC を自動で取るようにした後): **合格。**
- 相手の MAC を知らなければ、最初の REQ を同報(宛先の MAC が ff:ff:ff:ff:ff:ff)で送り、REP に入った MAC を RC QP と BYE に使う。
  メッセージの [56..59] に宛先の IPv4 を足し、受け手は自分宛てでないものを捨てる。
- OptiPlex → Precision の client(MAC なし): NG 0 件、hash は両側で `e92cdeb4e6b0c81d`、server は BYE を受けて閉じた。
  遅延は前と同じ(write 8B 中央値 1,336ns、read 8B 1,646ns、FAA 1,638ns)。同報の REQ を ConnectX-4 Lx の受け側が受け取った。
- 宛先でない IP(192.168.101.99)へ MAC なしで接続: 5 秒で「接続できない(REP が来ない)」、戻り値 1。
  同じ L2 の Precision の server は同報の REQ を受けたはずだが、接続を受けなかった(宛先の IPv4 で捨てた)。
- `mt -t 4`(MAC なし): 4 本とも同時に接続、[1]〜[4] OK、FAA の合計 400,000、速さは前と同じ(write 2.82 / read 2.26 / FAA 2.26 Mops)。
- Precision → OptiPlex の client: MAC なし・MAC を渡す形の両方で NG 0 件、hash 一致。
- 終わった後、両方とも mlx5_core に戻した(Precision の `ens1f0np0` は始める前から IP が付いていなかったので、付けていない)。

記録(2026-11-09、立ち上げ・接続・切断・停止を待たない形(状態機械)にした後): **合格。**
- `bfm_init()` / `bfm_connect()` / `bfm_accept()` / `bfm_disconnect()` / `bfm_shutdown()` は待つところで `BFM_INPROGRESS` を返す。
  bfmem_test は 50µs おきに呼び直す(`do_init()` など)。FW のコマンドも出して完了を見るだけにした(`mlx5_cmd_run()`)。
- OptiPlex → Precision の client(通常・`-x`): NG 0 件、hash は両側で `e92cdeb4e6b0c81d`。遅延は前と同じ
  (write 8B 中央値 1,336ns、read 8B 1,649ns、FAA 1,637ns)。`-x` の [7] は REMOTE_ACCESS で断られた。
- 宛先でない IP へ: 5 秒で「接続できない(REP が来ない)」、作りかけの QP / CQ を消して戻り値 1。
- `mt -t 4`(OptiPlex → Precision)と `mt -t 16`(Precision → OptiPlex): 全部の接続が同時に張れ、[1]〜[4] OK。
  速さも前と同じ(16 本: write 10.93 / read 8.56 / FAA 2.55 Mops)。FW のコマンドの入口を 16 本の確立で取り合っても進んだ。
- Precision → OptiPlex の client: NG 0 件、hash 一致。
- **最初の版では Precision の FW に渡すページが 7,841 → 5,680 に減った。** ページ要求に答えている間に来た 2 つめの要求
  (2,161 ページ)を受け流していた(FW は出し直さない)。答え終えてから答えるようにして 7,841 に戻した
  (その後 FW は 2,161 ページの返却を申し出る。前の版も同じ量を渡していた)。OptiPlex は 3,951(立ち上げの後の要求で 5,106)。
- 終わった後、両方とも mlx5_core に戻した。

記録(2026-11-10、代表 API `bfm_run()`・接続の表・2 ポート・双方向に作り直した後): **合格。**
- **2 本目のケーブル(OptiPlex `01:00.0` ↔ Precision `b3:00.1`)も繋がっていた**ので、両方の機械の 2 つの PF を全部 vfio-pci にして
  2 ポート × 16 本 = 32 本で試した(ポート 1 は 192.168.102.11 / .21。どちらも 25G)。Precision の VM・VF は無く、ConnectX の
  ポートに IP も付いていなかった。終わった後、4 つとも mlx5_core に戻した(リンクは 25G で UP。IP は元から付いていない)。
- 両方で同じ `run` を動かした(OptiPlex が掛け側)。`bfm_run()` を 4 本のスレッドから呼び、OptiPlex は 0.45 秒、Precision は
  約 2.6〜2.9 秒(FW の立ち上げが長い)で 32 本とも張れた。FW ページは OptiPlex 3,951 / ポート、Precision 7,841〜7,843 / ポート。
- 両方向とも一通りの検査 [1]〜[8] と mt の [1]〜[4](FAA の合計 3,200,000)が OK、hash は両方向で `e92cdeb4e6b0c81d`。
- `-x`(OptiPlex): 最後の接続(ポート 1 の 15)が REMOTE_ACCESS で断られて `BFM_ST_DOWN` になり、`bfm_reconnect()` で張り直して
  読めた。Precision は BYE を受けて `BFM_ST_DOWN` にし、`reconnect_down()` で自分の側を張り直した。

性能(`run`、`-n 100000`。mt は区間ごとに両方のノードが足並みをそろえて同時に打つ。速さは「全スレッドの量 ÷ 最も遅いスレッドの時間」)。
片方向は、受ける側に `-q` を付けて測った。OptiPlex は CPU 4 個(32 本のスレッドを重ねる)、Precision は 16 個。

| 2 ポート × 16 本 | 片方向 O→P | 片方向 P→O | 双方向 O→P | 双方向 P→O | 双方向の合計 |
|---|---|---|---|---|---|
| write 64KiB(MB/s)| 5,077 | 4,985 | 4,221(-x の回 3,952)| 5,453(5,359)| **9,674(77.4 Gbit/s)** |
| read 64KiB(MB/s)| 5,006 | 5,219 | 4,769(4,743)| 5,377(5,560)| **10,146(81.2 Gbit/s)** |
| write 8B(Mops)| 2.66 | 10.66 | 2.68 | 10.48 | 13.16 |
| read 8B(Mops)| 2.12 | 8.36 | 2.15 | 8.27 | 10.42 |
| FAA(Mops)| 2.18 | 2.54 | 1.74 | 2.48 | 4.22 |

(O→P = OptiPlex が打つ。read の行は打つ側が読む = データは逆向きに流れる。)

| 1 ポート × 16 本、双方向 | O→P | P→O | 合計 |
|---|---|---|---|
| write 64KiB(MB/s)| 2,597 | 2,826 | 5,423(43.4 Gbit/s)|
| read 64KiB(MB/s)| 2,869 | 2,882 | 5,751(46.0 Gbit/s)|
| write 8B / read 8B / FAA(Mops)| 2.69 / 2.08 / 2.10 | 10.61 / 8.37 / 2.54 | |

- **帯域は双方向でほぼ倍になる**(片方向 約 5.0〜5.2 GB/s → 双方向の合計 9.7〜10.3 GB/s)。1 ポートでも双方向の合計は
  5.4〜5.8 GB/s で、ポートの全二重(25G × 2)が効いている。2 ポートは 1 ポートのほぼ倍。
  片方向の 5.0〜5.2 GB/s は 2 ポートの線速(50 Gbit/s = 6.25 GB/s。経路 MTU 1024 のヘッダを除くと約 5.8 GB/s)の約 9 割。
  64KiB の write は 1 回ずつ ACK を待つ(中継バッファへ写してから送る)形なので、多重度を上げればまだ伸びる余地がある。
- 双方向で落ちたのは OptiPlex が打つ write 64KiB(5,077 → 4,221)と FAA(2.18 → 1.74)。OptiPlex は CPU 4 個で 32 本の
  スレッドの写しと、相手からの要求を受ける NIC のメモリの読み書きを同時にこなすため。Precision の側はほとんど落ちない。
- 8 バイトの Mops は打つ側の CPU で決まる(OptiPlex 約 2.7、Precision 約 10.5。双方向でも同じ)。FAA は受ける側で
  約 2.5 Mops で頭打ち(前と同じ)。
- 遅延(接続 (0, 0)、中央値): 双方向(相手も同時に一通りの検査を打っている)で write 8B 1,369〜1,379ns・read 8B 1,701〜1,704ns・
  FAA 1,712〜1,718ns。片方向では 1,339〜1,347 / 1,651〜1,654 / 1,645ns で、双方向にすると 30〜70ns 延びる。
  1 ポートの回の Precision は平均と p99 が約 10µs に延びた(中央値は同じ)。相手が同じポートで DIF のスループットを
  流している時間と重なったためとみられる(確かめていない)。

記録(2026-11-10、接続の数を変えたときの双方向の性能): **全部の回で mt の [1]〜[4] が OK。**
- 両方で `run -P -n 200000 -c <本数>` を動かした(`-P` は一通りの検査を飛ばす。8 バイトの区間は 1 本あたり 200,000 回、
  64KiB の区間は 4,000 回 = 256MiB)。区間ごとに両方のノードが足並みをそろえて同時に打つ。O = OptiPlex(CPU 4 個)、P = Precision(16 個)。
- 途中で試験プログラムの不具合を見つけて直した: `-c` を付けると、ポート 1 の相手の IP を書いてもポート 1 の本数が 0 のままだった
  (2 ポートの回を 1 回目に流したら 1 ポートと同じ結果になった。前の 32 本の回は `-c` を付けていないので影響なし)。

1 ポート(接続の数 = 合計の本数)。各欄は「O が打つ / P が打つ / 合計」。

| 本数 | write 64KiB(MB/s)| read 64KiB(MB/s)| write 8B(Mops)| read 8B(Mops)| FAA(Mops)|
|---|---|---|---|---|---|
| 1 | 2,228 / 2,228 / **4,456** | 1,966 / 1,966 / **3,932** | 0.73 / 0.73 / 1.46 | 0.59 / 0.59 / 1.18 | 0.58 / 0.58 / 1.16 |
| 2 | 2,719 / 2,719 / **5,438** | 2,889 / 2,889 / **5,778** | 1.44 / 1.44 / 2.88 | 1.14 / 1.14 / 2.28 | 1.12 / 1.12 / 2.24 |
| 3 | 2,698 / 2,699 / 5,397 | 2,889 / 2,889 / 5,778 | 2.09 / 2.14 / 4.23 | 1.53 / 1.69 / 3.22 | 1.66 / 1.67 / 3.33 |
| 4 | 2,689 / 2,750 / 5,439 | 2,888 / 2,872 / 5,760 | 2.61 / 2.84 / 5.45 | 2.16 / 2.23 / 4.39 | 1.67 / 2.18 / 3.85 |
| 5 | 2,508 / 2,752 / 5,260 | 2,650 / 2,885 / 5,535 | 1.80 / 3.53 / 5.33 | 1.44 / 2.77 / 4.21 | 1.41 / 2.54 / 3.95 |
| 6 | 2,684 / 2,773 / 5,457 | 2,881 / 2,889 / 5,770 | 2.12 / 4.21 / 6.33 | 1.68 / 3.30 / 4.98 | 1.64 / 2.54 / 4.18 |
| 7 | 2,686 / 2,786 / 5,472 | 2,882 / 2,884 / 5,766 | 2.45 / 4.90 / 7.35 | 1.95 / 3.85 / 5.80 | 1.81 / 2.55 / 4.36 |
| 8 | 2,391 / 2,797 / 5,188 | 2,884 / 2,742 / 5,626 | 2.70 / 5.55 / 8.25 | 2.18 / 4.36 / 6.54 | 1.78 / 2.54 / 4.32 |

2 ポート(各ポート 1〜4 本、合計 2〜8 本)。

| 合計の本数 | write 64KiB(MB/s)| read 64KiB(MB/s)| write 8B(Mops)| read 8B(Mops)| FAA(Mops)|
|---|---|---|---|---|---|
| 2(1 + 1)| 4,393 / 4,394 / **8,787** | 3,922 / 3,930 / **7,852** | 1.40 / 1.41 / 2.81 | 1.13 / 1.13 / 2.26 | 1.10 / 1.10 / 2.20 |
| 4(2 + 2)| 5,110 / 5,445 / **10,555** | 5,588 / 5,649 / **11,237** | 2.60 / 2.78 / 5.38 | 1.95 / 2.20 / 4.15 | 2.08 / 2.16 / 4.24 |
| 6(3 + 3)| 4,021 / 5,567 / 9,588 | 4,541 / 5,739 / 10,280 | 2.08 / 4.14 / 6.22 | 1.68 / 3.27 / 4.95 | 1.64 / 2.51 / 4.15 |
| 8(4 + 4)| 4,005 / 5,593 / 9,598 | 4,898 / 5,453 / 10,351 | 2.51 / 5.49 / 8.00 | 2.14 / 4.32 / 6.46 | 2.06 / 2.51 / 4.57 |

- **帯域(64KiB)は少ない本数で頭打ちになる。** 1 ポートでは 2 本で双方向の合計 5.4〜5.8 GB/s(43〜46 Gbit/s。全二重の 50 Gbit/s の
  約 9 割)に達し、それより増やしても変わらない。1 本では 4.5 / 3.9 GB/s(64KiB を 1 回ずつ ACK まで待つので、1 本では線が空く)。
  2 ポートでは各 2 本(合計 4 本)で最も高く、双方向の合計 10.6 / 11.2 GB/s(85〜90 Gbit/s)。同じ 4 本でも 1 ポートにまとめると 5.4 / 5.8 GB/s
  なので、**ポートを分けると帯域はほぼ倍**になる。各 3 本以上では OptiPlex が打つ側が 4.0 GB/s 前後に落ち、合計 9.6 / 10.3 GB/s に下がる
  (OptiPlex の CPU 4 個に、スレッド 6〜8 本・`bfm_run()` を回すスレッド・中継バッファへの写しが重なるため)。
- **8 バイトの操作は、本数にほぼ比例して伸びる(帯域ではなく往復の時間で決まる)。** 1 本あたり write 約 0.7 Mops(1 回 約 1.4µs)、
  read 約 0.55 Mops。Precision が打つ側は 8 本まで比例して伸びる(write 5.5 Mops)。OptiPlex が打つ側は CPU の数の 4 本で約 2.6 Mops に
  達し、5〜6 本では CPU より多いスレッドが取り合って一度下がる(1.8〜2.1 Mops)。同じ合計の本数なら、1 ポートでも 2 ポートでもほぼ同じ
  (線は空いているので、ポートを分けても速くならない)。
- FAA は、Precision が打つ側(OptiPlex のメモリへ)が 5 本以上で約 2.5 Mops で頭打ち(受ける OptiPlex の側の上限。前の記録と同じ)。
  OptiPlex が打つ側は CPU で 1.6〜2.1 Mops に止まる。
- 両方が同じ本数で CPU に余裕がある間(1〜3 本)は、両方向の数字がほぼ一致する(同じ線を同じだけ使い合うため)。

記録(2026-11-10、多重度つきの write / read(`bfm_write_async()` / `bfm_read_async()` / `bfm_test()` / `bfm_check()`)を足した後): **合格。**
- 2 ポート × 16 本、双方向、OptiPlex に `-x`: 両方向とも一通りの検査 [1]〜[9] と mt の [1]〜[4] が OK、hash は両方向で一致。
  [9]: ACK を確かめる write 200 個(64 を超えて出し、刈り取りながら出す)と read 200 個を tag ごとに確かめて中身が一致、
  使い回された古い tag が `BFM_OK`、write の直後の read がその中身を読む、ACK を確かめない write 2,000 個の後に `bfm_check()` で
  仕掛かりが 0 になり中身が一致。
- `-x` の [7]: 公開メモリの外への ACK を確かめない write の失敗(REMOTE_ACCESS)を `bfm_check()` が返した。時間切れを 0ms にすると
  `bfm_check()` が `BFM_E_TIMEOUT` を返して接続をエラーにした(実際の 100ms の時間切れは、相手が応答を止める形では起こしていない)。
  どちらの後も `bfm_reconnect()` で張り直して読めた(相手は BYE を受けて張り直した)。
- 2 ポート × 16 本、双方向の速さ(合計。O = OptiPlex が打つ / P = Precision が打つ):

  | 区間 | 1 回ずつ待つ | 多重度 64 |
  |---|---|---|
  | write 8B | O 2.63 / P 10.56 Mops | ACK なし O 33.84 / P 32.43 Mops、ACK あり O 24.01 / P 46.89 Mops |
  | read 8B | O 2.12 / P 8.28 Mops | O 22.64 / P 31.98 Mops |
  | write 帯域 | 64KiB: O 4,415 / P 5,650 MB/s | 4KiB: O 4,695 / P 5,331 MB/s |
  | read 帯域 | 64KiB: O 4,286 / P 5,437 MB/s | 4KiB: O 4,635 / P 4,844 MB/s |

多重度ごと(1 ポート、接続 1 本、双方向、`run -P -n 200000 -c 1 -Q <多重度>`。各欄は O / P):

| 多重度 | write 8B ACK なし(Mops)| write 8B ACK あり(Mops)| read 8B(Mops)| write 4KiB(MB/s)| read 4KiB(MB/s)|
|---|---|---|---|---|---|
| 1 | 0.75 / 0.73 | 0.75 / 0.73 | 0.60 / 0.59 | 1,048 / 1,049 | 989 / 1,045 |
| 2 | 1.47 / 1.44 | 1.47 / 1.44 | 1.18 / 1.16 | 1,720 / 1,718 | 1,739 / 1,942 |
| 4 | 2.91 / 2.86 | 2.91 / 2.85 | 2.00 / 1.98 | 2,675 / 2,662 | 2,532 / 2,831 |
| 8 | 5.36 / 5.12 | 5.36 / 5.12 | 3.55 / 3.54 | 2,677 / 2,676 | 2,816 / 2,825 |
| 16 | 8.06 / 6.46 | 8.03 / 6.47 | 6.20 / 5.96 | 2,688 / 2,705 | 2,826 / 2,825 |
| 32 | 8.98 / 6.80 | 8.93 / 6.80 | 7.40 / 6.69 | 2,685 / 2,708 | 2,826 / 2,825 |
| 64 | 9.55 / 6.92 | 9.51 / 6.93 | 7.44 / 6.87 | 2,682 / 2,708 | 2,826 / 2,825 |

- 8 バイトは多重度 8 までほぼ比例して伸び(1 → 8 で 7 倍)、1 本の接続・1 つのコアで最大 9.5 Mops(1 回 105ns)。多重度 1 は
  1 回ずつ待つ `bfm_write()` と同じ(0.73〜0.75 Mops)。16 を超えると伸びは緩い(1 つのコアで WQE を組んで出す速さで決まる)。
  Precision が打つ側は 16 以上で 6.5〜6.9 Mops に止まる(Precision の CPU の 1 コアの速さとみられる。確かめていない)。
- ACK を確かめる / 確かめないで速さは変わらない(どちらも完了通知を求め、刈り取りで枠を空けるため。違いは tag の有無と
  確かめ方だけ)。
- read 8B は相手が受け付ける同時の数(16)を NIC が守るので、16 を超えると伸びが止まる(7.4 Mops)。
- 4KiB は多重度 4 で 1 ポートの片方向の線速近く(2.7〜2.8 GB/s = 21〜23 Gbit/s)に達する。両方向の合計は 5.3〜5.7 GB/s。
- 仕様書の記述を書いたときの見直しで、張り直すと通し番号が 1 に戻り、張り直しの前に失敗した操作の古い tag が `bfm_test()` で
  `BFM_OK` になりうることが分かった。通し番号を張り直しをまたいで続け、張り直しの前の tag には `BFM_E_STATE` を返すように直した
  (あわせて通し番号 0 を飛ばし、別のエラーで使えない接続では時間切れより先にそのエラーを返す)。直した版で 2 ポート × 16 本・
  双方向・`-x` を流し直し、[7] の「張り直しの前に出した操作の tag は、張り直した後は BFM_E_STATE」を含めて全部 OK、hash も一致。

記録(2026-11-10、CAS / FAA の多重度つき(`bfm_cas_async()` / `bfm_faa_async()` / `bfm_cas32_async()` / `bfm_faa32_async()`)を足した後): **合格。**
- 2 ポート × 16 本、双方向: 両方向とも一通りの検査 [1]〜[9] と mt の [1]〜[4] が OK、hash は両方向で一致。
  [9] の atomic: FAA / FAA32 を同じ語へ 200 個(多重度 64 を超えて)出しておくと、元の値が 0〜199 の順に並び、語は 200 になる。
  CAS / CAS32 の i→i+1 を 200 個出しておくと全部成功する(相手の NIC は同じ接続の atomic を出した順に処理する)。
  元の値のバイト順(8 バイトは CQE ならビッグエンディアン・中継バッファならホスト順、4 バイトはどちらもビッグエンディアン)を
  非同期でも同期版と同じに扱って、どれも正しい値になった。
- 32 本・双方向の atomic の合計: OptiPlex が打つ側 1.51 Mops(FAA / CAS とも)、Precision が打つ側 2.48〜2.50 Mops。
- 多重度ごと(1 ポート、接続 1 本、双方向、`run -P -n 200000 -c 1 -Q <多重度>`。O / P):

  | 多重度 | FAA(Mops)| CAS(Mops)| FAA 64 個の語へ(Mops)|
  |---|---|---|---|
  | 1 | 0.58 / 0.57 | 0.57 / 0.57 | 0.57 / 0.57 |
  | 2 | 1.15 / 1.14 | 1.15 / 1.14 | |
  | 4 | 2.15 / 2.23 | 2.15 / 2.23 | 2.18 / 2.19 |
  | 8 | 2.17 / 2.62 | 2.17 / 2.62 | |
  | 16 | 2.14 / 2.65 | 2.14 / 2.65 | 2.28 / 2.55 |
  | 32 | 2.13 / 2.62 | 2.13 / 2.63 | |
  | 64 | 2.12 / 2.62 | 2.12 / 2.62 | 2.26 / 2.56 |

- **atomic は多重度 4 で頭打ち**(OptiPlex が打つ = Precision の NIC が受ける側で約 2.1、Precision が打つ = OptiPlex の NIC が
  受ける側で約 2.6 Mops。1 回 380〜470ns)。同じ語へ打っても 64 個の語(別のキャッシュライン)へ散らしても変わらないので、
  同じ語の順番待ちではなく、受ける NIC が atomic を処理する速さ(ホストのメモリを読んで書き戻す 1 往復)が上限とみられる。
  32 本に増やしても合計は増えない(上の 2.48 Mops)。read の 8 バイトが多重度 64 で 7.4 Mops まで伸びるのと違う。

記録(2026-11-11、わざと ACK を返さない試験(`run -z`。[10] 無応答)を足した後): **合格。**
- 両方に `-z`(OptiPlex には `-x` も)、2 ポート × 16 本、`-P -n 20000`。黙る側の Precision が、ポート 0 の接続 13〜15 の QP を
  BYE を送らずに消した(状態を「相手が切った」にして張り直しを頼むと、BYE を送らずに QP と CQ を消し、REQ 待ちに戻る)。
  消された QP 宛てのパケットは NIC が黙って捨てるので、試す側の OptiPlex には ACK も応答も返らない。
  - A: ACK を確かめる write で 64 個の枠が全部仕掛かり中になり、65 個目は資源の空きを **100.0 ms** 待って `BFM_E_TIMEOUT`。
    接続はエラーになり、仕掛かり中の tag も `BFM_E_TIMEOUT` を返した。
  - B: ACK を確かめない write 10 個の後、`bfm_check()` が **100.0 ms** で `BFM_E_TIMEOUT` を報告した(仕掛かり 10 個)。
  - C: 応答の返らない read の `bfm_wait()` が **100.0 ms** で `BFM_E_TIMEOUT` を返した。
  - その後、3 本とも `bfm_reconnect()` で張り直して読めた(黙る側は REQ を待っていた)。続けて `-x` の [7] も全部 OK。
- **試験プログラムの不具合を見つけて直した**: mt の速さの区間を 13 個に増やしたので、区間ごとの足並みの段(10 + 区間 = 10〜22)が
  「全部の接続での転送」の段 20 と重なり、相手の mt が終わる前に先へ進んでいた(1 回目の `-z` で、黙る側が試す側の mt の
  最後の区間の最中に QP を消し、その区間が時間切れになって見つかった)。段を 100 + 区間・200・210・300 に離し、重ならないことを
  `_Static_assert` で確かめるようにした。区間を 12・13 個にしてから流した回(CAS / FAA の多重度の回)は、段 20 の後に相手の
  mt と重なる試験が無かったので、結果には影響していない。
