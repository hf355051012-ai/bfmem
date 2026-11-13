/* 移植層: x86-64 の VxWorks(論理アドレス = 物理アドレス)。
 *
 * **この版は未検証(VxWorks の機材で動かしていない)。** VxWorks 固有の関数は使わない
 * (使うのは C の vprintf だけ)。時刻と待ちは TSC で作る(port/x86/bfm_x86.c。
 * TSC の周波数は CPUID か MSR から求め、求められなければ g_bfm_tsc_hz に入れる)。
 *
 * bfmem はカーネル(DKM)のタスクから使うこと。MSR を読む rdmsr 命令はリング 0 でしか使えず、
 * 物理アドレスをそのまま触るのもカーネルでしかできない。
 *
 * 前提(BSP 側で用意すること):
 *   - g_bfm_dma_* と g_bfm_mr_* の物理メモリが OS に使われず、論理 = 物理・ライトバックで
 *     張られていること(x86 の DMA はキャッシュと一貫しているので、キャッシュの操作は要らない)。
 *   - 使うポートごとに、BAR0(g_bfm_bar0_addr[] から BAR0 の大きさぶん)が論理 = 物理・キャッシュしない属性で
 *     張られていること。
 *   - BlueFlame を使うなら、BAR0 全体を write-combining(PAT の WC)で張った別名を用意し、
 *     ポートごとにその先頭を g_bfm_bar0_wc_va[] に入れること。無ければ 8 バイトのドアベルだけで動く。
 *   - g_bfm_cpu_awake はこの版では何もしない。相手からの read / atomic の遅延を抑えるには、
 *     BIOS / BSP で CPU の深い C-state(パッケージの C-state、C1E など)を止めること
 *     (g_bfm_verbose = 1 で立ち上げると、MSR から読んだパッケージ C-state の上限を表示する)。
 *   - PCIe の設定空間は ECAM(g_bfm_pcie_ecam_base)を直接読み書きする(ポートごとの g_bfm_pci_bus / dev / func[])。
 *
 * 待ち(bfm_port_delay_us)は TSC を見ながらの空回りで、CPU を他のタスクに譲らない。
 * 本体の立ち上げ・接続・切断・停止は待たない(時刻を見て BFM_INPROGRESS を返す)ので、待ちは使わない
 * (操作の完了は、もともと空回りで待つ)。 */
#include "bfm_port.h"
#include "bfm_config.h"
#include "bfm_x86.h"

#include <stdio.h>
#include <stdarg.h>

void bfm_port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* TSC から作る(周波数は bfm_port_init() で決める)。 */
uint64_t bfm_port_time_ns(void)
{
    return bfm_x86_time_ns();
}

void bfm_port_delay_us(uint32_t us)
{
    bfm_x86_delay_us(us);
}

/* VxWorks の関数(vxCpuIndexGet など)は使わず、CPU から APIC ID を読む。 */
unsigned bfm_port_cpu_id(void)
{
    return bfm_x86_apic_id();
}

/* リング 0 なので rdmsr 命令で直接読む(読んでよい MSR かは bfm_x86.c が CPUID で確かめる)。 */
static int vx_rdmsr(uint32_t msr, uint64_t *val)
{
    *val = bfm_x86_rdmsr(msr);
    return 0;
}

int bfm_port_init(void)
{
    if (g_bfm_pcie_ecam_base == 0) {
        bfm_port_log("port: g_bfm_pcie_ecam_base が 0(ECAM の位置を入れること)\n");
        return -1;
    }
    return bfm_x86_init(vx_rdmsr);
}

void bfm_port_fini(void)
{
}

static volatile uint32_t *ecam(unsigned port, uint32_t off)
{
    const uint64_t a = g_bfm_pcie_ecam_base + ((uint64_t)g_bfm_pci_bus[port] << 20) +
                       ((uint64_t)g_bfm_pci_dev[port] << 15) + ((uint64_t)g_bfm_pci_func[port] << 12) +
                       (off & 0xffcu);
    return (volatile uint32_t *)(uintptr_t)a;
}

/* 設定空間はリトルエンディアンなので、x86 ではそのまま読み書きできる。 */
uint32_t bfm_port_pci_read32(unsigned port, uint32_t off)
{
    return *ecam(port, off);
}

void bfm_port_pci_write32(unsigned port, uint32_t off, uint32_t val)
{
    *ecam(port, off) = val;
    bfm_mb();
}

static uint64_t s_bar_size[BFM_MAX_PORT];   /* bfm_port_bar0_wc() の範囲の検査に使う(ポートごと)*/

volatile uint8_t *bfm_port_bar0_map(unsigned port, uint64_t bar_phys, uint64_t bar_size)
{
    s_bar_size[port] = bar_size;
    return (volatile uint8_t *)(uintptr_t)bar_phys;     /* 論理 = 物理 */
}

void bfm_port_bar0_unmap(unsigned port)
{
    s_bar_size[port] = 0;
}

volatile uint8_t *bfm_port_bar0_wc(unsigned port, uint64_t off, uint64_t len)
{
    if (g_bfm_bar0_wc_va[port] == 0) return NULL;
    if (off > s_bar_size[port] || len > s_bar_size[port] - off) {
        bfm_port_log("port: ポート %u の WC の範囲 0x%llx+0x%llx が BAR0 の外\n", port,
                     (unsigned long long)off, (unsigned long long)len);
        return NULL;
    }
    return (volatile uint8_t *)(uintptr_t)(g_bfm_bar0_wc_va[port] + off);
}

void bfm_port_bar0_wc_release(unsigned port, uint64_t off, uint64_t len)
{
    (void)port;
    (void)off;
    (void)len;
}
