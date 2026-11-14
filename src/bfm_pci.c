/* PCIe の設定空間を自分で設定する(VxWorks では OS もドライバも設定しないため)。
 *
 *   1. 装置の確認(Mellanox の ConnectX-4〜7 の PF か)
 *   2. FLR(g_bfm_pcie_flr。前回の実行で動いたままの NIC を止める)
 *   3. BAR0 の大きさを測り、g_bfm_bar0_addr を書き込む(0 なら今の値を使う)
 *   4. Memory Space / Bus Master を有効にし、INTx を止める(割り込みは使わない)
 *   5. Max Read Request Size(g_bfm_pcie_mrrs)
 *
 * 設定空間の読み書きはポート層(bfm_port_pci_read32 / write32)を通す。Linux では
 * VFIO の設定空間を通るので、BAR への書き込みは VFIO の中だけで効く(実際の BAR は
 * カーネルが割り当てたまま)。そのため Linux では g_bfm_bar0_addr[] を 0 にしておく。
 * ConnectX はポートごとに別の function なので、ここの手順はポート(bfm_dev_t)ごとに進める。 */
#include "bfm_internal.h"

#define PCI_ID          0x00u
#define PCI_COMMAND     0x04u   /* 下位 16bit。上位は Status */
#define PCI_BAR0        0x10u
#define PCI_CAP_PTR     0x34u
#define PCI_CMD_MEM     0x0002u
#define PCI_CMD_MASTER  0x0004u
#define PCI_CMD_INTX_OFF 0x0400u
#define PCI_STATUS_CAP  0x0010u
#define PCI_CAP_ID_EXP  0x10u
#define PCIE_DEVCAP     0x04u
#define PCIE_DEVCTL     0x08u   /* 下位 16bit。上位は DevSta */
#define PCIE_LNKSTA     0x10u   /* 上位 16bit(LnkCtl の後ろ)*/
#define PCIE_DEVCAP_FLR 0x10000000u
#define PCIE_DEVCTL_FLR 0x8000u
#define PCIE_DEVSTA_TRPND 0x0020u

/* 対応する ConnectX(PCI のデバイス ID と名前)。ベンダ ID は MLX5_VENDOR_ID。 */
static const struct {
    uint16_t id;                     /* PCI のデバイス ID */
    const char *name;                /* 装置の名前(表示用)*/
} s_ids[] = {
    { 0x1013u, "ConnectX-4" },
    { 0x1015u, "ConnectX-4 Lx" },
    { 0x1017u, "ConnectX-5" },
    { 0x1019u, "ConnectX-5 Ex" },
    { 0x101bu, "ConnectX-6" },
    { 0x101du, "ConnectX-6 Dx" },
    { 0x101fu, "ConnectX-6 Lx" },
    { 0x1021u, "ConnectX-7" },
};

static uint16_t pci_cmd_read(bfm_dev_t *d)
{
    return (uint16_t)bfm_port_pci_read32(d->port, PCI_COMMAND);
}

/* Status(上位 16bit)は書いて 1 を消すビットなので、0 を書いて触らない。 */
static void pci_cmd_write(bfm_dev_t *d, uint16_t cmd)
{
    bfm_port_pci_write32(d->port, PCI_COMMAND, cmd);
}

/* PCI Express の capability の位置(無ければ 0)。 */
static uint32_t pci_find_pcie_cap(bfm_dev_t *d)
{
    if (!((bfm_port_pci_read32(d->port, PCI_COMMAND) >> 16) & PCI_STATUS_CAP)) return 0;
    uint32_t pos = bfm_port_pci_read32(d->port, PCI_CAP_PTR) & 0xfcu;
    for (unsigned guard = 0; pos != 0 && guard < 48u; guard++) {
        const uint32_t v = bfm_port_pci_read32(d->port, pos);
        if ((v & 0xffu) == PCI_CAP_ID_EXP) return pos;
        pos = (v >> 8) & 0xfcu;
    }
    return 0;
}

#define TRPND_WAIT_MS   100u    /* 保留中の DMA(Transactions Pending)が消えるのを待つ上限 */
#define FLR_WAIT_MS     100u    /* FLR をかけてから設定空間を読むまで(PCIe 基本仕様の決まり)*/
#define PRESENT_WAIT_MS 1000u   /* FLR の後、設定空間が読めるようになるのを待つ上限 */

static uint64_t ms_since(uint64_t t0)
{
    return (bfm_port_time_ns() - t0) / 1000000u;
}

/* 設定の状態(bfm_pci_setup_step() が 1 つずつ進める)。 */
enum {
    P_PROBE = 0,      /* 装置を確かめ、FLR をかけるなら Bus Master を落とす */
    P_TRPND,          /* 保留中の DMA が消えるのを待ってから FLR をかける */
    P_FLR_WAIT,       /* FLR から 100ms 待つ */
    P_PRESENT,        /* 設定空間が読めるようになるのを待つ */
    P_BAR,            /* BAR0・MRRS・Command を設定する */
    P_DONE            /* 済んだ */
};

static struct {
    int step;                        /* 今の状態(P_*)*/
    uint32_t pcie;                   /* PCI Express の capability の位置 */
    uint64_t t0;                     /* 時間で待つ状態の起点 */
} s_pci[BFM_MAX_PORT];                 /* ポートごと */

static int pci_setup(bfm_dev_t *d);

/*=================================================================
 * BAR0 の大きさを測り、addr が 0 でなければ書き込む。BAR0 の値(物理アドレス)と
 * 大きさを返す。測る間は Memory Space を止める。
 * ===============================================================*/
static int pci_setup_bar0(bfm_dev_t *d, uint64_t addr, uint64_t *out_phys, uint64_t *out_size)
{
    const uint32_t lo = bfm_port_pci_read32(d->port, PCI_BAR0);
    const uint32_t hi = bfm_port_pci_read32(d->port, PCI_BAR0 + 4u);
    if ((lo & 0x1u) != 0 || ((lo >> 1) & 0x3u) != 0x2u) {
        BFM_LOG("bfm: BAR0 が 64bit のメモリ BAR ではない (0x%08x)\n", lo);
        return -1;
    }
    const uint16_t cmd = pci_cmd_read(d);
    pci_cmd_write(d, (uint16_t)(cmd & ~(PCI_CMD_MEM | PCI_CMD_MASTER)));
    bfm_port_pci_write32(d->port, PCI_BAR0, 0xffffffffu);
    bfm_port_pci_write32(d->port, PCI_BAR0 + 4u, 0xffffffffu);
    const uint64_t mask = ((uint64_t)bfm_port_pci_read32(d->port, PCI_BAR0 + 4u) << 32) |
                          (bfm_port_pci_read32(d->port, PCI_BAR0) & ~0xfu);
    const uint64_t size = ~mask + 1u;
    uint64_t phys = ((uint64_t)hi << 32) | (lo & ~0xfu);
    const char *err = NULL;
    if (mask == 0 || (size & (size - 1u)) != 0) err = "BAR0 の大きさが測れない";
    else if (addr != 0 && (addr & (size - 1u)) != 0) err = "g_bfm_bar0_addr が BAR0 の大きさの境界に合っていない";
    if (err != NULL) {
        BFM_LOG("bfm: %s(g_bfm_bar0_addr[%u] = 0x%llx、BAR0 の大きさ 0x%llx)\n",
                err, d->port, (unsigned long long)addr, (unsigned long long)size);
        bfm_port_pci_write32(d->port, PCI_BAR0, lo);
        bfm_port_pci_write32(d->port, PCI_BAR0 + 4u, hi);
        pci_cmd_write(d, cmd);
        return -1;
    }
    if (addr != 0) phys = addr;
    bfm_port_pci_write32(d->port, PCI_BAR0, (uint32_t)phys | (lo & 0xfu));
    bfm_port_pci_write32(d->port, PCI_BAR0 + 4u, (uint32_t)(phys >> 32));
    pci_cmd_write(d, cmd);
    *out_phys = phys;
    *out_size = size;
    return 0;
}

/* 装置を確かめる(Mellanox の ConnectX-4〜7 の PF か)。d の vendor / device / name を埋める。 */
static int pci_probe(bfm_dev_t *d)
{
    const uint32_t id = bfm_port_pci_read32(d->port, PCI_ID);
    d->vendor = (uint16_t)id;
    d->device = (uint16_t)(id >> 16);
    d->name = NULL;
    for (unsigned i = 0; i < sizeof(s_ids) / sizeof(s_ids[0]); i++) {
        if (s_ids[i].id == d->device) d->name = s_ids[i].name;
    }
    if (d->vendor != MLX5_VENDOR_ID || d->name == NULL) {
        BFM_LOG("bfm: 対応していない装置 (vendor=0x%04x device=0x%04x)。"
                "ConnectX-4〜7 の PF(VF ではないもの)を指定すること\n", d->vendor, d->device);
        return -1;
    }
    s_pci[d->port].pcie = pci_find_pcie_cap(d);
    if (s_pci[d->port].pcie == 0) {
        BFM_LOG("bfm: PCI Express の capability が見つからない\n");
        return -1;
    }
    return 0;
}

/* 設定の状態を最初に戻す(ポートの立ち上げを始めるときに呼ぶ)。 */
void bfm_pci_setup_reset(bfm_dev_t *d)
{
    s_pci[d->port].step = P_PROBE;
    s_pci[d->port].pcie = 0;
    s_pci[d->port].t0 = 0;
}

/*=================================================================
 * PCIe の設定一式を、待つところまで進める。d の vendor / device / name / bar0_phys /
 * bar0_size を埋める。FLR(g_bfm_pcie_flr)は、保留中の DMA が終わるのを待ってから出し、
 * 100ms 待ち(PCIe 基本仕様の決まり)、設定空間が読めるようになるまで待つ。待つところでは
 * BFM_INPROGRESS を返す。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。BFM_E_HW = 失敗。
 * ===============================================================*/
int bfm_pci_setup_step(bfm_dev_t *d)
{
    for (;;) {
        switch (s_pci[d->port].step) {
        case P_PROBE:
            if (pci_probe(d) != 0) return BFM_E_HW;
            if (!g_bfm_pcie_flr) {
                s_pci[d->port].step = P_BAR;
            } else if (!(bfm_port_pci_read32(d->port, s_pci[d->port].pcie + PCIE_DEVCAP) & PCIE_DEVCAP_FLR)) {
                BFM_LOG("bfm: この装置は FLR に対応していない。リセットせずに進む\n");
                s_pci[d->port].step = P_BAR;
            } else {
                pci_cmd_write(d, (uint16_t)(pci_cmd_read(d) & ~PCI_CMD_MASTER));
                s_pci[d->port].t0 = bfm_port_time_ns();
                s_pci[d->port].step = P_TRPND;
            }
            break;
        case P_TRPND: {
            if ((bfm_port_pci_read32(d->port, s_pci[d->port].pcie + PCIE_DEVCTL) >> 16) & PCIE_DEVSTA_TRPND) {
                if (ms_since(s_pci[d->port].t0) < TRPND_WAIT_MS) return BFM_INPROGRESS;
                BFM_LOG("bfm: 注意: 100ms 待っても Transactions Pending が消えない。このまま FLR をかける\n");
            }
            const uint32_t ctl = bfm_port_pci_read32(d->port, s_pci[d->port].pcie + PCIE_DEVCTL) & 0xffffu;
            bfm_port_pci_write32(d->port, s_pci[d->port].pcie + PCIE_DEVCTL, ctl | PCIE_DEVCTL_FLR);
            s_pci[d->port].t0 = bfm_port_time_ns();
            s_pci[d->port].step = P_FLR_WAIT;
            break;
        }
        case P_FLR_WAIT:
            if (ms_since(s_pci[d->port].t0) < FLR_WAIT_MS) return BFM_INPROGRESS;
            s_pci[d->port].t0 = bfm_port_time_ns();
            s_pci[d->port].step = P_PRESENT;
            break;
        case P_PRESENT:
            if ((bfm_port_pci_read32(d->port, PCI_ID) & 0xffffu) != MLX5_VENDOR_ID) {
                if (ms_since(s_pci[d->port].t0) <= PRESENT_WAIT_MS) return BFM_INPROGRESS;
                BFM_LOG("bfm: FLR の後、設定空間が読めない\n");
                return BFM_E_HW;
            }
            BFM_DBG("bfm: FLR をかけた\n");
            s_pci[d->port].step = P_BAR;
            break;
        case P_BAR:
            if (pci_setup(d) != 0) return BFM_E_HW;
            s_pci[d->port].step = P_DONE;
            break;
        default:   /* P_DONE */
            return BFM_OK;
        }
    }
}

/*=================================================================
 * BAR0(g_bfm_bar0_addr)・MRRS(g_bfm_pcie_mrrs)・Command(Memory Space・Bus Master・
 * INTx を止める)を設定し、装置の情報を表示する。
 * ===============================================================*/
static int pci_setup(bfm_dev_t *d)
{
    const uint32_t pcie = s_pci[d->port].pcie;
    if (pci_setup_bar0(d, g_bfm_bar0_addr[d->port], &d->bar0_phys, &d->bar0_size) != 0) return -1;

    if (g_bfm_pcie_mrrs != 0) {
        unsigned code = 0;
        while (code < 5u && (128u << code) < g_bfm_pcie_mrrs) code++;
        uint32_t ctl = bfm_port_pci_read32(d->port, pcie + PCIE_DEVCTL) & 0xffffu;
        ctl = (ctl & ~0x7000u) | ((uint32_t)code << 12);
        bfm_port_pci_write32(d->port, pcie + PCIE_DEVCTL, ctl);
    }

    pci_cmd_write(d, (uint16_t)(pci_cmd_read(d) | PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF));

    const uint32_t lnk = bfm_port_pci_read32(d->port, pcie + PCIE_LNKSTA) >> 16;
    BFM_LOG("bfm: ポート %u: %s (%04x:%04x) BAR0=0x%llx (%llu MiB) PCIe Gen%u x%u MRRS=%u\n",
            d->port, d->name, d->vendor, d->device, (unsigned long long)d->bar0_phys,
            (unsigned long long)(d->bar0_size >> 20), lnk & 0xfu, (lnk >> 4) & 0x3fu,
            128u << ((bfm_port_pci_read32(d->port, pcie + PCIE_DEVCTL) >> 12) & 0x7u));
    return 0;
}

/* Bus Master を入れる / 落とす(落とすと NIC はメモリへ DMA しない)。 */
void bfm_pci_bus_master(bfm_dev_t *d, int on)
{
    const uint16_t cmd = pci_cmd_read(d);
    pci_cmd_write(d, on ? (uint16_t)(cmd | PCI_CMD_MASTER) : (uint16_t)(cmd & ~PCI_CMD_MASTER));
}

/*=================================================================
 * カードのリセット(bfm_fw_reset() から。MFRL に level 3 を書いた後)
 *
 * FW は、MFRL に level 3(PCI のリセット)を書かれた後に PCIe のリンクが落ちると、カード全体
 * (全部の function)をリセットし、次の起動の FW の像で立ち上がる。リンクは上流のポート
 * (ルートポートか PCIe スイッチの下りポート)で落とす(Linux の mlx5 の mlx5_pci_link_toggle() /
 * mlx5_pci_reset_bus() と同じ):
 *   Link Disable:        Link Control の Link Disable を 500ms 立てて戻す
 *   Secondary Bus Reset: Bridge Control の Secondary Bus Reset を 2ms 立てて戻す
 * どちらにするかは MFRL の pci_reset_req_method で FW が決める。この後、リンクが上がる
 * (Data Link Layer Link Active)のを待ち、100ms 待ち、設定空間が読めるようになるのを待って、
 * リセットで消えた設定空間(BAR・Command・Max Payload Size など)を戻す。
 * カードの全部の function がリセットされるので、使っている全部のポートの設定空間を覚えて戻す
 * (別のカードのポートは、同じ値を書き直すだけ)。OS が設定空間を仮想化していても装置そのものへ
 * 書くため、bfm_port_pci_hw_read32 / write32 を使う。
 * ===============================================================*/

#define PCI_HDR_TYPE        0x0cu   /* 下位 16bit が cache line・latency、その上が header type と BIST */
#define PCI_BRIDGE_CTL      0x3cu   /* 上位 16bit が Bridge Control(Type 1)*/
#define PCI_BCTL_SBR        0x00400000u   /* Secondary Bus Reset(dword 0x3c の bit 22)*/
#define PCIE_LNKCAP         0x0cu
#define PCIE_LNKCTL         0x10u   /* 下位 16bit。上位は LnkSta */
#define PCIE_DEVCTL2        0x28u
#define PCIE_LNKCAP_DLLLARC 0x00100000u   /* Data Link Layer Link Active を報告できる */
#define PCIE_LNKCTL_LD      0x0010u
#define PCIE_LNKCTL_RL      0x0020u
#define PCIE_LNKSTA_DLLLA   0x2000u
#define PCIE_TYPE_ROOT      0x4u    /* PCIe の Device/Port Type: ルートポート */
#define PCIE_TYPE_DOWN      0x6u    /* PCIe の Device/Port Type: スイッチの下りポート */

#define RST_LD_HOLD_MS      500u    /* Link Disable を立てておく時間(Linux の mlx5 と同じ)*/
#define RST_SBR_HOLD_MS     2u      /* Secondary Bus Reset を立てておく時間(Linux の pci_reset_secondary_bus() と同じ)*/
#define RST_LINK_WAIT_MS    2000u   /* リンクが上がるのを待つ上限 */
#define RST_NO_DLLLA_MS     1000u   /* Link Active を報告できない上流のポートでは、この時間だけ待つ */
#define RST_SETTLE_MS       100u    /* リンクが上がってから設定空間を読むまで(PCIe 基本仕様の決まり)*/
#define RST_READY_WAIT_MS   2000u   /* 設定空間が読めるようになるのを待つ上限 */
#define RST_NBAR            6u      /* 覚えて戻す BAR の数(0x10〜0x24)*/

/* リセットの状態(bfm_pci_reset_step() が 1 つずつ進める)。 */
enum {
    L_HOLD = 0,       /* Link Disable / Secondary Bus Reset を立てて待つ */
    L_LINK,           /* リンクが上がるのを待つ */
    L_SETTLE,         /* 100ms 待つ */
    L_READY,          /* 全部のポートの設定空間が読めるのを待ち、設定を戻す */
    L_DONE            /* 済んだ */
};

static struct {
    int step;                        /* 今の状態(L_*)*/
    int method;                      /* BFM_RESET_LINK_DISABLE / BFM_RESET_HOT_RESET */
    uint32_t up_pcie;                /* 上流のポートの PCI Express の capability の位置 */
    int dllla;                       /* 1 = 上流のポートが Link Active を報告できる */
    uint64_t t0;                     /* 時間で待つ状態の起点 */
    uint64_t t_start;                /* リンクを戻した時刻(表示用)*/
    struct {
        uint32_t pcie;               /* 装置の PCI Express の capability の位置(0 = 覚えていない)*/
        uint32_t cmd, hdr, bar[RST_NBAR], devctl, lnkctl, devctl2;   /* 覚えた設定空間 */
    } dev[BFM_MAX_PORT];
} s_rst;

/* PCI Express の capability の位置(up = 1 なら上流のポート、0 なら装置そのもの。無ければ 0)。 */
static uint32_t rst_find_pcie(unsigned port, int up)
{
    const uint32_t st = up ? bfm_port_up_read32(port, PCI_COMMAND) : bfm_port_pci_hw_read32(port, PCI_COMMAND);
    if (st == 0xffffffffu || !((st >> 16) & PCI_STATUS_CAP)) return 0;
    uint32_t pos = (up ? bfm_port_up_read32(port, PCI_CAP_PTR) : bfm_port_pci_hw_read32(port, PCI_CAP_PTR)) & 0xfcu;
    for (unsigned guard = 0; pos != 0 && guard < 48u; guard++) {
        const uint32_t v = up ? bfm_port_up_read32(port, pos) : bfm_port_pci_hw_read32(port, pos);
        if ((v & 0xffu) == PCI_CAP_ID_EXP) return pos;
        pos = (v >> 8) & 0xfcu;
    }
    return 0;
}

/*=================================================================
 * ポート d の上流のポートで、カードのリセット(リンクを落とす)ができるかを確かめる(待たない)。
 * 上流のポートが無い・PCIe のルートポートでもスイッチの下りポートでもないなら断る。
 * 戻り値: BFM_OK = できる。BFM_E_NOTSUP = できない(理由を表示する)。
 * ===============================================================*/
int bfm_pci_reset_check(bfm_dev_t *d)
{
    const unsigned p = d->port;
    const uint32_t id = bfm_port_up_read32(p, PCI_ID);
    if ((id & 0xffffu) == 0xffffu) {
        BFM_LOG("bfm: ポート %u: 上流のポートの設定空間が読めない (カードのリセットはできない)\n", p);
        return BFM_E_NOTSUP;
    }
    const uint32_t hdr = (bfm_port_up_read32(p, PCI_HDR_TYPE) >> 16) & 0x7fu;
    const uint32_t pos = rst_find_pcie(p, 1);
    const uint32_t type = pos ? (bfm_port_up_read32(p, pos) >> 20) & 0xfu : 0u;
    if (hdr != 1u || pos == 0 || (type != PCIE_TYPE_ROOT && type != PCIE_TYPE_DOWN)) {
        BFM_LOG("bfm: ポート %u: 上流の装置 (%04x:%04x) が PCIe のルートポートでもスイッチの下りポートでもない "
                "(header %u、型 %u)\n", p, id & 0xffffu, id >> 16, hdr, type);
        return BFM_E_NOTSUP;
    }
    s_rst.up_pcie = pos;
    s_rst.dllla = (bfm_port_up_read32(p, pos + PCIE_LNKCAP) & PCIE_LNKCAP_DLLLARC) != 0;
    return BFM_OK;
}

/*=================================================================
 * カードのリセットを始める: 使っている全部のポートの設定空間を覚え、ポート d の上流のポートで
 * リンクを落とす(method = MFRL の pci_reset_req_method)。bfm_pci_reset_check() が通った後、
 * 全部のポートを止めてから(Bus Master を落としてから)呼ぶ。続きは bfm_pci_reset_step()。
 * ===============================================================*/
void bfm_pci_reset_begin(bfm_dev_t *d, int method)
{
    const unsigned p = d->port;
    for (unsigned q = 0; q < BFM_MAX_PORT; q++) {
        const uint32_t pos = g_bfm_dev[q].used ? rst_find_pcie(q, 0) : 0u;
        s_rst.dev[q].pcie = pos;
        if (pos == 0) continue;
        s_rst.dev[q].cmd = bfm_port_pci_hw_read32(q, PCI_COMMAND) & 0xffffu;
        s_rst.dev[q].hdr = bfm_port_pci_hw_read32(q, PCI_HDR_TYPE) & 0xffffu;   /* cache line・latency だけ(BIST は書かない)*/
        for (unsigned i = 0; i < RST_NBAR; i++) s_rst.dev[q].bar[i] = bfm_port_pci_hw_read32(q, PCI_BAR0 + 4u * i);
        s_rst.dev[q].devctl = bfm_port_pci_hw_read32(q, pos + PCIE_DEVCTL) & 0xffffu & ~PCIE_DEVCTL_FLR;
        s_rst.dev[q].lnkctl = bfm_port_pci_hw_read32(q, pos + PCIE_LNKCTL) & 0xffffu & ~(PCIE_LNKCTL_LD | PCIE_LNKCTL_RL);
        s_rst.dev[q].devctl2 = bfm_port_pci_hw_read32(q, pos + PCIE_DEVCTL2) & 0xffffu;
    }
    s_rst.method = method;
    if (method == BFM_RESET_HOT_RESET) {
        BFM_LOG("bfm: ポート %u: 上流のポートで Secondary Bus Reset をかけ、カードをリセットする\n", p);
        bfm_port_up_write32(p, PCI_BRIDGE_CTL, bfm_port_up_read32(p, PCI_BRIDGE_CTL) | PCI_BCTL_SBR);
    } else {
        BFM_LOG("bfm: ポート %u: 上流のポートでリンクを切り (Link Disable)、カードをリセットする\n", p);
        const uint32_t ctl = bfm_port_up_read32(p, s_rst.up_pcie + PCIE_LNKCTL) & 0xffffu;
        bfm_port_up_write32(p, s_rst.up_pcie + PCIE_LNKCTL, ctl | PCIE_LNKCTL_LD);
    }
    s_rst.t0 = bfm_port_time_ns();
    s_rst.step = L_HOLD;
}

/* 覚えた設定空間をポート q へ戻す(BAR などを先に、Command を最後に)。 */
static void rst_restore(unsigned q)
{
    const uint32_t pos = s_rst.dev[q].pcie;
    for (unsigned i = 0; i < RST_NBAR; i++) bfm_port_pci_hw_write32(q, PCI_BAR0 + 4u * i, s_rst.dev[q].bar[i]);
    bfm_port_pci_hw_write32(q, PCI_HDR_TYPE, s_rst.dev[q].hdr);
    bfm_port_pci_hw_write32(q, pos + PCIE_DEVCTL, s_rst.dev[q].devctl);   /* 上位(DevSta)は 0 を書いて触らない */
    bfm_port_pci_hw_write32(q, pos + PCIE_LNKCTL, s_rst.dev[q].lnkctl);
    bfm_port_pci_hw_write32(q, pos + PCIE_DEVCTL2, s_rst.dev[q].devctl2);
    bfm_port_pci_hw_write32(q, PCI_COMMAND, s_rst.dev[q].cmd);
}

/*=================================================================
 * カードのリセットを、待つところまで進める: リンクを戻す -> リンクが上がるのを待つ -> 100ms 待つ ->
 * 全部のポートの設定空間が読めるのを待つ -> 覚えた設定空間を戻す。待つところでは BFM_INPROGRESS を返す。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。BFM_E_TIMEOUT = リンクが上がらない・設定空間が読めない。
 * ===============================================================*/
int bfm_pci_reset_step(bfm_dev_t *d)
{
    const unsigned p = d->port;
    for (;;) {
        switch (s_rst.step) {
        case L_HOLD:
            if (ms_since(s_rst.t0) < (s_rst.method == BFM_RESET_HOT_RESET ? RST_SBR_HOLD_MS : RST_LD_HOLD_MS))
                return BFM_INPROGRESS;
            if (s_rst.method == BFM_RESET_HOT_RESET) {
                bfm_port_up_write32(p, PCI_BRIDGE_CTL, bfm_port_up_read32(p, PCI_BRIDGE_CTL) & ~PCI_BCTL_SBR);
            } else {
                const uint32_t ctl = bfm_port_up_read32(p, s_rst.up_pcie + PCIE_LNKCTL) & 0xffffu;
                bfm_port_up_write32(p, s_rst.up_pcie + PCIE_LNKCTL, ctl & ~PCIE_LNKCTL_LD);
            }
            s_rst.t0 = s_rst.t_start = bfm_port_time_ns();
            s_rst.step = L_LINK;
            break;
        case L_LINK:
            if (!s_rst.dllla) {
                if (ms_since(s_rst.t0) < RST_NO_DLLLA_MS) return BFM_INPROGRESS;
            } else if (!((bfm_port_up_read32(p, s_rst.up_pcie + PCIE_LNKCTL) >> 16) & PCIE_LNKSTA_DLLLA)) {
                if (ms_since(s_rst.t0) < RST_LINK_WAIT_MS) return BFM_INPROGRESS;
                BFM_LOG("bfm: ポート %u: カードのリセットの後、%u ms 待ってもリンクが上がらない\n", p, RST_LINK_WAIT_MS);
                s_rst.step = L_DONE;
                return BFM_E_TIMEOUT;
            }
            BFM_LOG("bfm: ポート %u: リンクが上がった (%llu ms)\n", p, (unsigned long long)ms_since(s_rst.t_start));
            s_rst.t0 = bfm_port_time_ns();
            s_rst.step = L_SETTLE;
            break;
        case L_SETTLE:
            if (ms_since(s_rst.t0) < RST_SETTLE_MS) return BFM_INPROGRESS;
            s_rst.t0 = bfm_port_time_ns();
            s_rst.step = L_READY;
            break;
        case L_READY:
            for (unsigned q = 0; q < BFM_MAX_PORT; q++) {
                if (s_rst.dev[q].pcie == 0) continue;
                if ((bfm_port_pci_hw_read32(q, PCI_ID) & 0xffffu) == MLX5_VENDOR_ID) continue;
                if (ms_since(s_rst.t0) < RST_READY_WAIT_MS) return BFM_INPROGRESS;
                BFM_LOG("bfm: ポート %u: カードのリセットの後、%u ms 待っても設定空間が読めない\n", q, RST_READY_WAIT_MS);
                s_rst.step = L_DONE;
                return BFM_E_TIMEOUT;
            }
            for (unsigned q = 0; q < BFM_MAX_PORT; q++) {
                if (s_rst.dev[q].pcie == 0) continue;
                rst_restore(q);
                BFM_DBG("bfm: ポート %u: 設定空間を戻した (BAR0 0x%08x%08x、Command 0x%04x)\n", q,
                        bfm_port_pci_hw_read32(q, PCI_BAR0 + 4u), bfm_port_pci_hw_read32(q, PCI_BAR0),
                        bfm_port_pci_hw_read32(q, PCI_COMMAND) & 0xffffu);
            }
            BFM_LOG("bfm: ポート %u: カードのリセットが済んだ (リンクを戻してから %llu ms)\n", p,
                    (unsigned long long)ms_since(s_rst.t_start));
            s_rst.step = L_DONE;
            return BFM_OK;
        default:   /* L_DONE */
            return BFM_OK;
        }
    }
}
