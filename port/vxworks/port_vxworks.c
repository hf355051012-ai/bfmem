/* 移植層: x86-64 の VxWorks(論理アドレス = 物理アドレス)。
 *
 * **この版は未検証(VxWorks の機材で動かしていない)。** 使う VxWorks の API は
 * printf・tick64Get・sysClkRateGet・taskDelay だけにしてある。
 *
 * 前提(BSP 側で用意すること):
 *   - g_bfm_dma_* と g_bfm_mr_* の物理メモリが OS に使われず、論理 = 物理・ライトバックで
 *     張られていること(x86 の DMA はキャッシュと一貫しているので、キャッシュの操作は要らない)。
 *   - BAR0(g_bfm_bar0_addr から BAR0 の大きさぶん)が論理 = 物理・キャッシュしない属性で
 *     張られていること。
 *   - BlueFlame を使うなら、BAR0 全体を write-combining(PAT の WC)で張った別名を用意し、
 *     その先頭を g_bfm_bar0_wc_va に入れること。無ければ 8 バイトのドアベルだけで動く。
 *   - g_bfm_cpu_awake はこの版では何もしない。相手からの read / atomic の遅延を抑えるには、
 *     BIOS / BSP で CPU の深い C-state(パッケージの C-state、C1E など)を止めること。
 *   - PCIe の設定空間は ECAM(g_bfm_pcie_ecam_base)を直接読み書きする。 */
#include "bfm_port.h"
#include "bfm_config.h"

#include <vxWorks.h>
#include <stdio.h>
#include <stdarg.h>
#include <tickLib.h>
#include <sysLib.h>
#include <taskLib.h>

void bfm_port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* 分解能は 1 tick(時間切れの判定にしか使わない)。tick64Get() は 64bit なので一周しない。 */
uint64_t bfm_port_time_ns(void)
{
    const uint64_t hz = (uint64_t)sysClkRateGet();
    return (uint64_t)tick64Get() * (1000000000ull / (hz ? hz : 60u));
}

void bfm_port_delay_us(uint32_t us)
{
    const uint64_t hz = (uint64_t)sysClkRateGet();
    uint64_t ticks = ((uint64_t)us * hz + 999999u) / 1000000u;
    if (ticks == 0) ticks = 1;
    (void)taskDelay((int)ticks);
}

int bfm_port_init(void)
{
    if (g_bfm_pcie_ecam_base == 0) {
        bfm_port_log("port: g_bfm_pcie_ecam_base が 0(ECAM の位置を入れること)\n");
        return -1;
    }
    return 0;
}

void bfm_port_fini(void)
{
}

static volatile uint32_t *ecam(uint32_t off)
{
    const uint64_t a = g_bfm_pcie_ecam_base + ((uint64_t)g_bfm_pci_bus << 20) +
                       ((uint64_t)g_bfm_pci_dev << 15) + ((uint64_t)g_bfm_pci_func << 12) + (off & 0xffcu);
    return (volatile uint32_t *)(uintptr_t)a;
}

/* 設定空間はリトルエンディアンなので、x86 ではそのまま読み書きできる。 */
uint32_t bfm_port_pci_read32(uint32_t off)
{
    return *ecam(off);
}

void bfm_port_pci_write32(uint32_t off, uint32_t val)
{
    *ecam(off) = val;
    bfm_mb();
}

static uint64_t s_bar_size;   /* bfm_port_bar0_wc() の範囲の検査に使う */

volatile uint8_t *bfm_port_bar0_map(uint64_t bar_phys, uint64_t bar_size)
{
    s_bar_size = bar_size;
    return (volatile uint8_t *)(uintptr_t)bar_phys;     /* 論理 = 物理 */
}

void bfm_port_bar0_unmap(void)
{
    s_bar_size = 0;
}

volatile uint8_t *bfm_port_bar0_wc(uint64_t off, uint64_t len)
{
    if (g_bfm_bar0_wc_va == 0) return NULL;
    if (off > s_bar_size || len > s_bar_size - off) {
        bfm_port_log("port: WC の範囲 0x%llx+0x%llx が BAR0 の外\n",
                     (unsigned long long)off, (unsigned long long)len);
        return NULL;
    }
    return (volatile uint8_t *)(uintptr_t)(g_bfm_bar0_wc_va + off);
}

void bfm_port_bar0_wc_release(uint64_t off, uint64_t len)
{
    (void)off;
    (void)len;
}
