/* 移植層: x86-64 Linux + VFIO(試験環境)。
 *
 * VxWorks の「論理アドレス = 物理アドレス」を真似るため、DMA 領域と公開メモリを
 * **g_bfm_dma_base / g_bfm_mr_base の仮想アドレスにそのまま張り、同じ値を IOVA にして
 * VFIO_IOMMU_MAP_DMA する**。これで NIC に渡すアドレス = CPU のポインタの値になり、
 * ライブラリの本体は VxWorks と同じ書き方のまま動く。
 *
 * BAR0 は VFIO の fd ではなく sysfs の resource0 から張る。VFIO の fd から張ると
 * vfio-pci がカーネルの中でも BAR 全体を UC で登録し、後から一部のページを
 * write-combining にできなくなる(x86 の PAT は同じ物理ページに別の型を許さず、黙って
 * UC- に格下げする)。BlueFlame に使う UAR のページは、窓を「前 / そのページ / 後」の
 * 3 つに張り直してからそのページを resource0_wc で張る(vfio_nvme で実機確認済みの手順。
 * 1 ページだけ munmap しても元の mmap が全部消えるまで PAT の登録が残るため)。
 *
 * 装置は vfio-pci に結びつけておくこと(README.md「試験」)。 */
#define _GNU_SOURCE
#include "bfm_port.h"
#include "bfm_config.h"
#include "bfm_x86.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int s_container = -1;        /* VFIO のコンテナ(/dev/vfio/vfio)の fd。全部のポートで 1 つ */
static int s_dma_latency_fd = -1;   /* /dev/cpu_dma_latency(開いている間だけ効く)*/
static int s_msr_fd = -1;           /* /dev/cpu/N/msr(modprobe msr が要る)*/
static void *s_dma_va;              /* DMA 領域を張った先(= g_bfm_dma_base)*/
static void *s_mr_va;               /* 公開メモリを張った先(= g_bfm_mr_base)*/

/* VFIO のグループ。同じカードの 2 つの PF は同じグループのことがある(OptiPlex)ので、
 * 開いたグループを覚えておき、同じ番号なら使い回す。 */
static struct {
    int id;                          /* IOMMU グループの番号 */
    int fd;                          /* /dev/vfio/<番号> の fd */
} s_grp[BFM_MAX_PORT];
static int s_ngrp;                   /* s_grp の使っている数 */

/* UC の窓を作っている mmap の一覧(BAR0 内のオフセット)。PAT の登録は mmap 単位で、
 * その mmap のページが全部外れるまで残るので、どの mmap に入っているかを覚えておく。 */
#define SEG_MAX ((int)(2u * BFM_MAX_PORT_CONN + 8u))   /* WC に張る UAR(接続の枠ごとに 1 つ)1 つで区間が最大 2 つ増える */

/* ポートごとの装置。 */
static struct {
    int device;                      /* VFIO の装置の fd(設定空間と BAR0 の読み書き。-1 = 掴んでいない)*/
    uint64_t cfg_off;                /* 装置の fd の中の設定空間の位置(VFIO の region の offset)*/
    uint8_t *bar0;                   /* BAR0 を張った先(UC)*/
    uint64_t bar0_size;              /* BAR0 の大きさ(VFIO から聞いた値)*/
    int bar0_sysfs;                  /* 1 = BAR0 を sysfs の resource0 から張った(WC に張り替えられる)*/
    uint64_t bar0_vfio_off;          /* 装置の fd の中の BAR0 の位置(sysfs で張れないときに使う)*/
    struct { uint64_t off, len; } seg[SEG_MAX];   /* UC の mmap の区間(off = BAR0 内の位置、len = 長さ)*/
    int nseg;                        /* seg の使っている数 */
} s_dev[BFM_MAX_PORT];

void bfm_port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

uint64_t bfm_port_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void bfm_port_delay_us(uint32_t us)
{
    struct timespec ts = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* OS の CPU の番号(taskset や /proc/cpuinfo の番号と同じ)。取れなければ APIC ID。 */
unsigned bfm_port_cpu_id(void)
{
    const int c = sched_getcpu();
    return (c >= 0) ? (unsigned)c : bfm_x86_apic_id();
}

/* ---------------------------------------------------------------------
 * CPU(port/x86/bfm_x86.c)
 *
 * Linux の時刻と待ちは OS のもの(clock_gettime・nanosleep)を使う。TSC の周波数の求め方と
 * MSR の読み方を VxWorks と同じコードで確かめるため、bfm_x86_init() も呼んでおく。
 * ユーザ空間では rdmsr 命令を使えないので、MSR は /dev/cpu/N/msr から読む。
 * ------------------------------------------------------------------- */

static int linux_rdmsr(uint32_t msr, uint64_t *val)
{
    return pread(s_msr_fd, val, sizeof(*val), (off_t)msr) == (ssize_t)sizeof(*val) ? 0 : -1;
}

static void cpu_init(void)
{
    char path[64];
    const int cpu = sched_getcpu();

    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu < 0 ? 0 : cpu);
    s_msr_fd = open(path, O_RDONLY);
    if (s_msr_fd < 0 && g_bfm_verbose) {
        bfm_port_log("port: %s を開けない (errno=%d)。MSR は読まない(modprobe msr)\n", path, errno);
    }
    if (bfm_x86_init(s_msr_fd >= 0 ? linux_rdmsr : NULL) != 0 || !g_bfm_verbose) return;

    /* TSC の周波数を、OS の時計で 100ms 測った値と比べる */
    {
        const uint64_t c0 = bfm_x86_rdtsc(), t0 = bfm_port_time_ns();
        uint64_t hz;
        bfm_port_delay_us(100000u);
        hz = (bfm_x86_rdtsc() - c0) * 1000000000ull / (bfm_port_time_ns() - t0);
        bfm_port_log("port: TSC を OS の時計で測ると %llu.%03llu MHz(CPU %d)\n",
                     (unsigned long long)(hz / 1000000u), (unsigned long long)(hz / 1000u % 1000u), cpu);
    }
}

/* ---------------------------------------------------------------------
 * VFIO
 * ------------------------------------------------------------------- */

/* コンテナを開く(最初のポートのときだけ)。 */
static int vfio_open_container(void)
{
    if (s_container >= 0) return 0;
    s_container = open("/dev/vfio/vfio", O_RDWR);
    if (s_container < 0 || ioctl(s_container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
        !ioctl(s_container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
        bfm_port_log("port: /dev/vfio/vfio が使えない (errno=%d)\n", errno);
        return -1;
    }
    return 0;
}

/* IOMMU グループ grp を開いてコンテナに入れる(開いてあれば使い回す)。戻り値: グループの fd(-1 = 失敗)。 */
static int vfio_open_group(int grp)
{
    char path[64];
    for (int i = 0; i < s_ngrp; i++)
        if (s_grp[i].id == grp) return s_grp[i].fd;
    snprintf(path, sizeof(path), "/dev/vfio/%d", grp);
    const int fd = open(path, O_RDWR);
    if (fd < 0) {
        bfm_port_log("port: %s が開けない (errno=%d)。vfio-pci に結びつけたか\n", path, errno);
        return -1;
    }
    s_grp[s_ngrp].id = grp;
    s_grp[s_ngrp].fd = fd;
    s_ngrp++;
    struct vfio_group_status st = { .argsz = sizeof(st) };
    if (ioctl(fd, VFIO_GROUP_GET_STATUS, &st) < 0 || !(st.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        bfm_port_log("port: IOMMU グループ %d が使えない(同じグループの装置を全部 vfio-pci に結びつけること)\n", grp);
        return -1;
    }
    if (ioctl(fd, VFIO_GROUP_SET_CONTAINER, &s_container) < 0) {
        bfm_port_log("port: IOMMU グループ %d をコンテナに入れられない (errno=%d)\n", grp, errno);
        return -1;
    }
    /* IOMMU の型は、最初のグループを入れた後に 1 回だけ決める */
    if (s_ngrp == 1 && ioctl(s_container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        bfm_port_log("port: VFIO のコンテナを作れない (errno=%d)\n", errno);
        return -1;
    }
    return fd;
}

/* ポート port の装置(g_bfm_linux_pci_bdf[port])を掴む。 */
static int vfio_open_device(unsigned port)
{
    const char *bdf = g_bfm_linux_pci_bdf[port];
    char path[256], link[256];
    if (bdf == NULL) {
        bfm_port_log("port: g_bfm_linux_pci_bdf[%u] が NULL(PCI の BDF を入れること)\n", port);
        return -1;
    }
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    const ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n < 0) {
        bfm_port_log("port: %s が読めない (errno=%d)。IOMMU が無効か、PCI アドレスが違う\n", path, errno);
        return -1;
    }
    link[n] = '\0';
    const char *slash = strrchr(link, '/');
    const int grp = atoi(slash ? slash + 1 : link);
    if (vfio_open_container() != 0) return -1;
    const int gfd = vfio_open_group(grp);
    if (gfd < 0) return -1;
    s_dev[port].device = ioctl(gfd, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (s_dev[port].device < 0) {
        bfm_port_log("port: %s を掴めない (errno=%d)\n", bdf, errno);
        return -1;
    }
    struct vfio_region_info ri = { .argsz = sizeof(ri), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    if (ioctl(s_dev[port].device, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) return -1;
    s_dev[port].cfg_off = ri.offset;
    struct vfio_region_info rb = { .argsz = sizeof(rb), .index = VFIO_PCI_BAR0_REGION_INDEX };
    if (ioctl(s_dev[port].device, VFIO_DEVICE_GET_REGION_INFO, &rb) < 0) return -1;
    s_dev[port].bar0_vfio_off = rb.offset;
    s_dev[port].bar0_size = rb.size;
    if (g_bfm_verbose) bfm_port_log("port: ポート %u = %s(IOMMU グループ %d)\n", port, bdf, grp);
    return 0;
}

/*=================================================================
 * va の位置に size バイトを張り(2MiB の境界・倍数なら hugepage を先に試す)、
 * 同じ値を IOVA にして NIC から見えるようにする(コンテナの中の全部の装置から見える)。
 * ===============================================================*/
static void *map_region(uint64_t va, uint64_t size, const char *what)
{
    void *p = MAP_FAILED;
    const int base = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_POPULATE;
    if (((va | size) & ((2u << 20) - 1u)) == 0)
        p = mmap((void *)(uintptr_t)va, size, PROT_READ | PROT_WRITE, base | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED)
        p = mmap((void *)(uintptr_t)va, size, PROT_READ | PROT_WRITE, base, -1, 0);
    if (p == MAP_FAILED || p != (void *)(uintptr_t)va) {
        bfm_port_log("port: %s を 0x%llx に張れない (errno=%d)。g_bfm_*_base を変えること\n",
                     what, (unsigned long long)va, errno);
        if (p != MAP_FAILED) munmap(p, size);
        return NULL;
    }
    struct vfio_iommu_type1_dma_map dm = {
        .argsz = sizeof(dm),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = va, .iova = va, .size = size,
    };
    if (ioctl(s_container, VFIO_IOMMU_MAP_DMA, &dm) < 0) {
        bfm_port_log("port: %s を IOVA 0x%llx へ対応づけられない (errno=%d)\n", what,
                     (unsigned long long)va, errno);
        munmap(p, size);
        return NULL;
    }
    return p;
}

int bfm_port_init(void)
{
    int used = 0;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        s_dev[p].device = -1;
        s_dev[p].bar0 = NULL;
        s_dev[p].nseg = 0;
    }
    s_ngrp = 0;
    /* 使うポートの装置を全部掴んでから(グループがコンテナに入ってから)DMA 領域を張る */
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        if (g_bfm_port_conn[p] == 0) continue;
        used = 1;
        if (vfio_open_device(p) != 0) {
            bfm_port_fini();
            return -1;
        }
    }
    if (!used) {
        bfm_port_log("port: 使うポートが無い(g_bfm_port_conn[] が全部 0)\n");
        return -1;
    }
    s_dma_va = map_region(g_bfm_dma_base, g_bfm_dma_size, "DMA 領域");
    s_mr_va = s_dma_va ? map_region(g_bfm_mr_base, g_bfm_mr_size, "公開メモリ") : NULL;
    if (s_dma_va == NULL || s_mr_va == NULL) {
        bfm_port_fini();
        return -1;
    }
    cpu_init();
    if (g_bfm_cpu_awake) {
        /* PM QoS: 0 を書いた fd を開いている間、CPU は深い C-state に入らない(閉じれば元に戻る)。 */
        const int32_t zero = 0;
        s_dma_latency_fd = open("/dev/cpu_dma_latency", O_WRONLY);
        if (s_dma_latency_fd < 0 || write(s_dma_latency_fd, &zero, sizeof(zero)) != (ssize_t)sizeof(zero)) {
            bfm_port_log("port: 注意: /dev/cpu_dma_latency に書けない (errno=%d)。相手からの read / atomic が遅くなりうる\n",
                         errno);
            if (s_dma_latency_fd >= 0) close(s_dma_latency_fd);
            s_dma_latency_fd = -1;
        }
    }
    return 0;
}

void bfm_port_fini(void)
{
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        if (s_dev[p].bar0) bfm_port_bar0_unmap(p);
        if (s_dev[p].device > 0)   /* 0 = まだ掴んでいない(静的な初期値)*/
            close(s_dev[p].device);
        s_dev[p].device = -1;
    }
    for (int i = 0; i < s_ngrp; i++) close(s_grp[i].fd);
    s_ngrp = 0;
    if (s_container >= 0) close(s_container);   /* IOMMU の対応づけもここで外れる */
    s_container = -1;
    if (s_dma_va) munmap(s_dma_va, g_bfm_dma_size);
    if (s_mr_va) munmap(s_mr_va, g_bfm_mr_size);
    s_dma_va = s_mr_va = NULL;
    if (s_dma_latency_fd >= 0) close(s_dma_latency_fd);
    s_dma_latency_fd = -1;
    if (s_msr_fd >= 0) close(s_msr_fd);
    s_msr_fd = -1;
}

uint32_t bfm_port_pci_read32(unsigned port, uint32_t off)
{
    uint32_t v = 0xffffffffu;
    if (pread(s_dev[port].device, &v, 4, (off_t)(s_dev[port].cfg_off + off)) != 4)
        bfm_port_log("port: ポート %u の設定空間 0x%x を読めない (errno=%d)\n", port, off, errno);
    return v;
}

void bfm_port_pci_write32(unsigned port, uint32_t off, uint32_t val)
{
    if (pwrite(s_dev[port].device, &val, 4, (off_t)(s_dev[port].cfg_off + off)) != 4)
        bfm_port_log("port: ポート %u の設定空間 0x%x へ書けない (errno=%d)\n", port, off, errno);
}

/* ---------------------------------------------------------------------
 * BAR0
 * ------------------------------------------------------------------- */

/* ポート port の BAR0 の [off, off+len) を UC(sysfs の resource0、だめなら VFIO)で va に張る。 */
static void *bar_map_uc(unsigned port, void *va, uint64_t off, uint64_t len, int fixed)
{
    void *p = MAP_FAILED;
    const int flags = MAP_SHARED | (fixed ? MAP_FIXED : 0);
    if (s_dev[port].bar0_sysfs) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", g_bfm_linux_pci_bdf[port]);
        const int fd = open(path, O_RDWR | O_SYNC);
        if (fd >= 0) {
            p = mmap(va, len, PROT_READ | PROT_WRITE, flags, fd, (off_t)off);
            close(fd);
        }
    } else {
        p = mmap(va, len, PROT_READ | PROT_WRITE, flags, s_dev[port].device,
                 (off_t)(s_dev[port].bar0_vfio_off + off));
    }
    return p;
}

volatile uint8_t *bfm_port_bar0_map(unsigned port, uint64_t bar_phys, uint64_t bar_size)
{
    (void)bar_phys;
    if (bar_size != 0 && bar_size != s_dev[port].bar0_size)
        bfm_port_log("port: 注意: ポート %u の BAR0 の大きさが VFIO(0x%llx)と設定空間(0x%llx)で違う\n", port,
                     (unsigned long long)s_dev[port].bar0_size, (unsigned long long)bar_size);
    s_dev[port].bar0_sysfs = 1;
    void *p = bar_map_uc(port, NULL, 0, s_dev[port].bar0_size, 0);
    if (p == MAP_FAILED) {
        bfm_port_log("port: sysfs の resource0 が張れない。VFIO から張る(BlueFlame は使えない)\n");
        s_dev[port].bar0_sysfs = 0;
        p = bar_map_uc(port, NULL, 0, s_dev[port].bar0_size, 0);
    }
    if (p == MAP_FAILED) return NULL;
    s_dev[port].bar0 = (uint8_t *)p;
    s_dev[port].nseg = 0;
    return s_dev[port].bar0;
}

void bfm_port_bar0_unmap(unsigned port)
{
    if (s_dev[port].bar0) munmap(s_dev[port].bar0, s_dev[port].bar0_size);
    s_dev[port].bar0 = NULL;
    s_dev[port].nseg = 0;
}

/* UC で張った区間を表に足す(表が一杯なら足さない。その区間は WC に張り替えられなくなるだけ)。 */
static void seg_add(unsigned port, uint64_t off, uint64_t len)
{
    if (s_dev[port].nseg >= SEG_MAX) return;
    s_dev[port].seg[s_dev[port].nseg].off = off;
    s_dev[port].seg[s_dev[port].nseg].len = len;
    s_dev[port].nseg++;
}

/*=================================================================
 * ポート port の BAR0 の [off, off+len) を同じ仮想アドレスのまま write-combining に張り替える。
 * そのページを含む mmap の前と後を張り直して、元の mmap をそのページだけに縮め、
 * そのページを外してから resource0_wc で張る。失敗したら UC に戻して NULL。
 * ===============================================================*/
volatile uint8_t *bfm_port_bar0_wc(unsigned port, uint64_t off, uint64_t len)
{
    uint8_t *const bar0 = s_dev[port].bar0;
    if (!bar0 || !s_dev[port].bar0_sysfs || off + len > s_dev[port].bar0_size) return NULL;
    if (s_dev[port].nseg == 0) seg_add(port, 0, s_dev[port].bar0_size);
    int k = -1;
    for (int i = 0; i < s_dev[port].nseg; i++)
        if (off >= s_dev[port].seg[i].off && off + len <= s_dev[port].seg[i].off + s_dev[port].seg[i].len) k = i;
    if (k < 0 || s_dev[port].nseg + 2 > SEG_MAX) return NULL;
    uint8_t *va = bar0 + off;
    const uint64_t s0 = s_dev[port].seg[k].off, s1 = s_dev[port].seg[k].off + s_dev[port].seg[k].len;
    if (off > s0 && bar_map_uc(port, bar0 + s0, s0, off - s0, 1) != (void *)(bar0 + s0)) return NULL;
    if (off + len < s1 && bar_map_uc(port, va + len, off + len, s1 - off - len, 1) != (void *)(va + len)) return NULL;
    /* ここまでで前後は別の mmap になった。表もそのとおりに分ける
     * (前後の張り直しに失敗したときは、全部 UC のままなので表は変えない)。 */
    s_dev[port].seg[k] = s_dev[port].seg[--s_dev[port].nseg];
    if (off > s0) seg_add(port, s0, off - s0);
    if (off + len < s1) seg_add(port, off + len, s1 - off - len);
    if (munmap(va, len) != 0) {
        seg_add(port, off, len);                             /* 外せなかった = UC のまま残っている */
        return NULL;
    }
    char path[128];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0_wc", g_bfm_linux_pci_bdf[port]);
    const int fd = open(path, O_RDWR | O_SYNC);
    void *p = MAP_FAILED;
    if (fd >= 0) {
        p = mmap(va, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)off);
        close(fd);
    }
    if (p == (void *)va) return va;
    bfm_port_log("port: %s が張れない (errno=%d)。UC に戻す\n", path, errno);
    if (bar_map_uc(port, va, off, len, 1) == (void *)va) seg_add(port, off, len);
    return NULL;
}

void bfm_port_bar0_wc_release(unsigned port, uint64_t off, uint64_t len)
{
    if (!s_dev[port].bar0 || off + len > s_dev[port].bar0_size) return;
    uint8_t *va = s_dev[port].bar0 + off;
    if (munmap(va, len) == 0 && bar_map_uc(port, va, off, len, 1) == (void *)va) seg_add(port, off, len);
}
