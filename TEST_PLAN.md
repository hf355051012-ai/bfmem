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

```bash
# 両方: ビルドして vfio-pci にする(OptiPlex は 01:00.0 と 01:00.1、Precision は b3:00.0)
make
echo vfio-pci | sudo tee /sys/bus/pci/devices/<BDF>/driver_override
echo <BDF> | sudo tee /sys/bus/pci/devices/<BDF>/driver/unbind
echo <BDF> | sudo tee /sys/bus/pci/drivers_probe

# Precision(受け側)
sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 server
# OptiPlex(掛け側)。"" / -B / -x の 3 通り
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 client 192.168.101.21 b8:ce:f6:73:bf:7e

# 役割を入れ替える: OptiPlex で server、Precision で client(と bench)
sudo build/bfmem_test -d 0000:01:00.1 -i 192.168.101.11 server
sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 client 192.168.101.11 b8:3f:d2:1c:eb:81
sudo build/bfmem_test -d 0000:b3:00.0 -i 192.168.101.21 -n 10000 bench 192.168.101.11 b8:3f:d2:1c:eb:81
```

終わったら driver_override を空にして unbind / drivers_probe で mlx5_core に戻す(段階 5 と同じ)。

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
