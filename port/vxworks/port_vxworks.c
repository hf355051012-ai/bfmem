/* 移植層: VxWorks(論理アドレス = 物理アドレス)。
 *
 * **この版は未検証(VxWorks の機材で動かしていない)。** 使う VxWorks の API は
 * printf・tickGet・sysClkRateGet・taskDelay・cacheFlush・cacheInvalidate だけにしてある。
 *
 * 前提(BSP 側で用意すること):
 *   - g_bfm_dma_* と g_bfm_mr_* の物理メモリが OS に使われず、論理 = 物理で張られていること
 *     (NIC と CPU のキャッシュが一貫しない CPU では、キャッシュしない属性で張るか、
 *      BFM_VX_CACHE_OPS を定義して cacheFlush / cacheInvalidate を使う)。
 *   - BAR0(g_bfm_bar0_addr から BAR0 の大きさぶん)が論理 = 物理・キャッシュしない属性で
 *     張られていること。
 *   - BlueFlame を使うなら、BAR0 全体を write-combining で張った別名を用意し、その先頭を
 *     g_bfm_bar0_wc_va に入れること(x86 なら PAT の WC、ARM64 なら Normal-NC か
 *     Device-GRE)。無ければ 8 バイトのドアベルだけで動く。
 *   - g_bfm_cpu_awake はこの版では何もしない。相手からの read / atomic の遅延を抑えるには、
 *     BIOS / BSP で CPU の深い C-state(パッケージの C-state、C1E など)を止めること。
 *   - PCIe の設定空間は ECAM(g_bfm_pcie_ecam_base)を直接読み書きする。ECAM が無い機材では
 *     BFM_VX_PCI_LIB を定義して pciConfigLib(pciConfigInLong / pciConfigOutLong)を使う。 */
#include "bfm_port.h"
#include "bfm_config.h"

#include <vxWorks.h>
#include <stdio.h>
#include <stdarg.h>
#include <tickLib.h>
#include <sysLib.h>
#include <taskLib.h>
#include <cacheLib.h>
#ifdef BFM_VX_PCI_LIB
#include <drv/pci/pciConfigLib.h>
#endif

void bfm_port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* 分解能は 1 tick(時間切れの判定にしか使わない)。 */
uint64_t bfm_port_time_ns(void)
{
    const uint64_t hz = (uint64_t)sysClkRateGet();
    return (uint64_t)tickGet() * (1000000000ull / (hz ? hz : 60u));
}

void bfm_port_delay_us(uint32_t us)
{
    const uint64_t hz = (uint64_t)sysClkRateGet();
    uint64_t ticks = ((uint64_t)us * hz + 999999u) / 1000000u;
    if (ticks == 0) ticks = 1;
    (void)taskDelay((int)ticks);
}

void bfm_port_cache_flush(const volatile void *p, size_t len)
{
#ifdef BFM_VX_CACHE_OPS
    (void)cacheFlush(DATA_CACHE, (void *)(uintptr_t)p, len);
#else
    (void)p;
    (void)len;
#endif
}

void bfm_port_cache_inval(const volatile void *p, size_t len)
{
#ifdef BFM_VX_CACHE_OPS
    (void)cacheInvalidate(DATA_CACHE, (void *)(uintptr_t)p, len);
#else
    (void)p;
    (void)len;
#endif
}

int bfm_port_init(void)
{
#ifndef BFM_VX_PCI_LIB
    if (g_bfm_pcie_ecam_base == 0) {
        bfm_port_log("port: g_bfm_pcie_ecam_base が 0(ECAM の位置を入れるか、BFM_VX_PCI_LIB で作り直すこと)\n");
        return -1;
    }
#endif
    return 0;
}

void bfm_port_fini(void)
{
}

#ifndef BFM_VX_PCI_LIB
static volatile uint32_t *ecam(uint32_t off)
{
    const uint64_t a = g_bfm_pcie_ecam_base + ((uint64_t)g_bfm_pci_bus << 20) +
                       ((uint64_t)g_bfm_pci_dev << 15) + ((uint64_t)g_bfm_pci_func << 12) + (off & 0xffcu);
    return (volatile uint32_t *)(uintptr_t)a;
}
#endif

/* 設定空間はリトルエンディアン。ECAM の読み書きもリトルエンディアンの 32bit として扱う。 */
uint32_t bfm_port_pci_read32(uint32_t off)
{
#ifdef BFM_VX_PCI_LIB
    UINT32 v = 0xffffffffu;
    (void)pciConfigInLong((int)g_bfm_pci_bus, (int)g_bfm_pci_dev, (int)g_bfm_pci_func, (int)off, &v);
    return v;
#else
    const uint32_t v = *ecam(off);
    return BFM_HOST_BIG_ENDIAN ? bfm_bswap32(v) : v;
#endif
}

void bfm_port_pci_write32(uint32_t off, uint32_t val)
{
#ifdef BFM_VX_PCI_LIB
    (void)pciConfigOutLong((int)g_bfm_pci_bus, (int)g_bfm_pci_dev, (int)g_bfm_pci_func, (int)off, val);
#else
    *ecam(off) = BFM_HOST_BIG_ENDIAN ? bfm_bswap32(val) : val;
    bfm_mb();
#endif
}

volatile uint8_t *bfm_port_bar0_map(uint64_t bar_phys, uint64_t bar_size)
{
    (void)bar_size;
    return (volatile uint8_t *)(uintptr_t)bar_phys;     /* 論理 = 物理 */
}

void bfm_port_bar0_unmap(void)
{
}

volatile uint8_t *bfm_port_bar0_wc(uint64_t off, uint64_t len)
{
    (void)len;
    if (g_bfm_bar0_wc_va == 0) return NULL;
    return (volatile uint8_t *)(uintptr_t)(g_bfm_bar0_wc_va + off);
}

void bfm_port_bar0_wc_release(uint64_t off, uint64_t len)
{
    (void)off;
    (void)len;
}
