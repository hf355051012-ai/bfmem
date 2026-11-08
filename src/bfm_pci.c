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
 * カーネルが割り当てたまま)。そのため Linux では g_bfm_bar0_addr を 0 にしておく。 */
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

static uint16_t pci_cmd_read(void)
{
    return (uint16_t)bfm_port_pci_read32(PCI_COMMAND);
}

/* Status(上位 16bit)は書いて 1 を消すビットなので、0 を書いて触らない。 */
static void pci_cmd_write(uint16_t cmd)
{
    bfm_port_pci_write32(PCI_COMMAND, cmd);
}

/* PCI Express の capability の位置(無ければ 0)。 */
static uint32_t pci_find_pcie_cap(void)
{
    if (!((bfm_port_pci_read32(PCI_COMMAND) >> 16) & PCI_STATUS_CAP)) return 0;
    uint32_t pos = bfm_port_pci_read32(PCI_CAP_PTR) & 0xfcu;
    for (unsigned guard = 0; pos != 0 && guard < 48u; guard++) {
        const uint32_t v = bfm_port_pci_read32(pos);
        if ((v & 0xffu) == PCI_CAP_ID_EXP) return pos;
        pos = (v >> 8) & 0xfcu;
    }
    return 0;
}

static int pci_wait_present(uint32_t ms)
{
    const uint64_t t0 = bfm_port_time_ns();
    while ((bfm_port_pci_read32(PCI_ID) & 0xffffu) != MLX5_VENDOR_ID) {
        if (bfm_port_time_ns() - t0 > (uint64_t)ms * 1000000u) return -1;
        bfm_port_delay_us(1000u);
    }
    return 0;
}

/*=================================================================
 * FLR をかける。保留中の DMA が終わるのを待ってから出し、100ms 待つ
 * (PCIe 基本仕様の決まり)。その後、設定空間が読めるようになるまで待つ。
 * ===============================================================*/
static int pci_flr(uint32_t pcie)
{
    if (!(bfm_port_pci_read32(pcie + PCIE_DEVCAP) & PCIE_DEVCAP_FLR)) {
        BFM_LOG("bfm: この装置は FLR に対応していない。リセットせずに進む\n");
        return 0;
    }
    pci_cmd_write((uint16_t)(pci_cmd_read() & ~PCI_CMD_MASTER));
    unsigned i;
    for (i = 0; i < 100u; i++) {
        if (!((bfm_port_pci_read32(pcie + PCIE_DEVCTL) >> 16) & PCIE_DEVSTA_TRPND)) break;
        bfm_port_delay_us(1000u);
    }
    if (i == 100u) BFM_LOG("bfm: 注意: 100ms 待っても Transactions Pending が消えない。このまま FLR をかける\n");
    const uint32_t ctl = bfm_port_pci_read32(pcie + PCIE_DEVCTL) & 0xffffu;
    bfm_port_pci_write32(pcie + PCIE_DEVCTL, ctl | PCIE_DEVCTL_FLR);
    bfm_port_delay_us(100000u);
    if (pci_wait_present(1000u) != 0) {
        BFM_LOG("bfm: FLR の後、設定空間が読めない\n");
        return -1;
    }
    BFM_DBG("bfm: FLR をかけた\n");
    return 0;
}

/*=================================================================
 * BAR0 の大きさを測り、addr が 0 でなければ書き込む。BAR0 の値(物理アドレス)と
 * 大きさを返す。測る間は Memory Space を止める。
 * ===============================================================*/
static int pci_setup_bar0(uint64_t addr, uint64_t *out_phys, uint64_t *out_size)
{
    const uint32_t lo = bfm_port_pci_read32(PCI_BAR0);
    const uint32_t hi = bfm_port_pci_read32(PCI_BAR0 + 4u);
    if ((lo & 0x1u) != 0 || ((lo >> 1) & 0x3u) != 0x2u) {
        BFM_LOG("bfm: BAR0 が 64bit のメモリ BAR ではない (0x%08x)\n", lo);
        return -1;
    }
    const uint16_t cmd = pci_cmd_read();
    pci_cmd_write((uint16_t)(cmd & ~(PCI_CMD_MEM | PCI_CMD_MASTER)));
    bfm_port_pci_write32(PCI_BAR0, 0xffffffffu);
    bfm_port_pci_write32(PCI_BAR0 + 4u, 0xffffffffu);
    const uint64_t mask = ((uint64_t)bfm_port_pci_read32(PCI_BAR0 + 4u) << 32) |
                          (bfm_port_pci_read32(PCI_BAR0) & ~0xfu);
    const uint64_t size = ~mask + 1u;
    uint64_t phys = ((uint64_t)hi << 32) | (lo & ~0xfu);
    const char *err = NULL;
    if (mask == 0 || (size & (size - 1u)) != 0) err = "BAR0 の大きさが測れない";
    else if (addr != 0 && (addr & (size - 1u)) != 0) err = "g_bfm_bar0_addr が BAR0 の大きさの境界に合っていない";
    if (err != NULL) {
        BFM_LOG("bfm: %s(g_bfm_bar0_addr = 0x%llx、BAR0 の大きさ 0x%llx)\n",
                err, (unsigned long long)addr, (unsigned long long)size);
        bfm_port_pci_write32(PCI_BAR0, lo);
        bfm_port_pci_write32(PCI_BAR0 + 4u, hi);
        pci_cmd_write(cmd);
        return -1;
    }
    if (addr != 0) phys = addr;
    bfm_port_pci_write32(PCI_BAR0, (uint32_t)phys | (lo & 0xfu));
    bfm_port_pci_write32(PCI_BAR0 + 4u, (uint32_t)(phys >> 32));
    pci_cmd_write(cmd);
    *out_phys = phys;
    *out_size = size;
    return 0;
}

/*=================================================================
 * PCIe の設定一式。d の vendor / device / name / bar0_phys / bar0_size を埋める。
 * ===============================================================*/
int bfm_pci_setup(bfm_dev_t *d)
{
    const uint32_t id = bfm_port_pci_read32(PCI_ID);
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
    const uint32_t pcie = pci_find_pcie_cap();
    if (pcie == 0) {
        BFM_LOG("bfm: PCI Express の capability が見つからない\n");
        return -1;
    }
    if (g_bfm_pcie_flr && pci_flr(pcie) != 0) return -1;

    if (pci_setup_bar0(g_bfm_bar0_addr, &d->bar0_phys, &d->bar0_size) != 0) return -1;

    if (g_bfm_pcie_mrrs != 0) {
        unsigned code = 0;
        while (code < 5u && (128u << code) < g_bfm_pcie_mrrs) code++;
        uint32_t ctl = bfm_port_pci_read32(pcie + PCIE_DEVCTL) & 0xffffu;
        ctl = (ctl & ~0x7000u) | ((uint32_t)code << 12);
        bfm_port_pci_write32(pcie + PCIE_DEVCTL, ctl);
    }

    pci_cmd_write((uint16_t)(pci_cmd_read() | PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF));

    const uint32_t lnk = bfm_port_pci_read32(pcie + PCIE_LNKSTA) >> 16;
    BFM_LOG("bfm: %s (%04x:%04x) BAR0=0x%llx (%llu MiB) PCIe Gen%u x%u MRRS=%u\n",
            d->name, d->vendor, d->device, (unsigned long long)d->bar0_phys,
            (unsigned long long)(d->bar0_size >> 20), lnk & 0xfu, (lnk >> 4) & 0x3fu,
            128u << ((bfm_port_pci_read32(pcie + PCIE_DEVCTL) >> 12) & 0x7u));
    return 0;
}

/* Bus Master を入れる / 落とす(落とすと NIC はメモリへ DMA しない)。 */
void bfm_pci_bus_master(int on)
{
    const uint16_t cmd = pci_cmd_read();
    pci_cmd_write(on ? (uint16_t)(cmd | PCI_CMD_MASTER) : (uint16_t)(cmd & ~PCI_CMD_MASTER));
}
