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

#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
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

static int s_container = -1, s_group = -1, s_device = -1;
static int s_dma_latency_fd = -1;   /* /dev/cpu_dma_latency(開いている間だけ効く)*/
static uint64_t s_cfg_off;
static uint8_t *s_bar0;
static uint64_t s_bar0_size;
static int s_bar0_sysfs;
static uint64_t s_bar0_vfio_off;
static void *s_dma_va, *s_mr_va;

/* UC の窓を作っている mmap の一覧(BAR0 内のオフセット)。PAT の登録は mmap 単位で、
 * その mmap のページが全部外れるまで残るので、どの mmap に入っているかを覚えておく。 */
#define SEG_MAX 32
static struct { uint64_t off, len; } s_seg[SEG_MAX];
static int s_nseg;

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

/* ---------------------------------------------------------------------
 * VFIO
 * ------------------------------------------------------------------- */

static int vfio_open_device(const char *bdf)
{
    char path[256], link[256];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    const ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n < 0) {
        bfm_port_log("port: %s が読めない (errno=%d)。IOMMU が無効か、PCI アドレスが違う\n", path, errno);
        return -1;
    }
    link[n] = '\0';
    const char *slash = strrchr(link, '/');
    const int grp = atoi(slash ? slash + 1 : link);

    s_container = open("/dev/vfio/vfio", O_RDWR);
    if (s_container < 0 || ioctl(s_container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
        !ioctl(s_container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
        bfm_port_log("port: /dev/vfio/vfio が使えない (errno=%d)\n", errno);
        return -1;
    }
    snprintf(path, sizeof(path), "/dev/vfio/%d", grp);
    s_group = open(path, O_RDWR);
    if (s_group < 0) {
        bfm_port_log("port: %s が開けない (errno=%d)。vfio-pci に結びつけたか\n", path, errno);
        return -1;
    }
    struct vfio_group_status st = { .argsz = sizeof(st) };
    if (ioctl(s_group, VFIO_GROUP_GET_STATUS, &st) < 0 || !(st.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        bfm_port_log("port: IOMMU グループ %d が使えない(同じグループの装置を全部 vfio-pci に結びつけること)\n", grp);
        return -1;
    }
    if (ioctl(s_group, VFIO_GROUP_SET_CONTAINER, &s_container) < 0 ||
        ioctl(s_container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        bfm_port_log("port: VFIO のコンテナを作れない (errno=%d)\n", errno);
        return -1;
    }
    s_device = ioctl(s_group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (s_device < 0) {
        bfm_port_log("port: %s を掴めない (errno=%d)\n", bdf, errno);
        return -1;
    }
    struct vfio_region_info ri = { .argsz = sizeof(ri), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    if (ioctl(s_device, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) return -1;
    s_cfg_off = ri.offset;
    struct vfio_region_info rb = { .argsz = sizeof(rb), .index = VFIO_PCI_BAR0_REGION_INDEX };
    if (ioctl(s_device, VFIO_DEVICE_GET_REGION_INFO, &rb) < 0) return -1;
    s_bar0_vfio_off = rb.offset;
    s_bar0_size = rb.size;
    return 0;
}

/*=================================================================
 * va の位置に size バイトを張り(2MiB の境界・倍数なら hugepage を先に試す)、
 * 同じ値を IOVA にして NIC から見えるようにする。
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
    if (g_bfm_linux_pci_bdf == NULL) {
        bfm_port_log("port: g_bfm_linux_pci_bdf が NULL(PCI の BDF を入れること)\n");
        return -1;
    }
    if (vfio_open_device(g_bfm_linux_pci_bdf) != 0) {
        bfm_port_fini();
        return -1;
    }
    s_dma_va = map_region(g_bfm_dma_base, g_bfm_dma_size, "DMA 領域");
    s_mr_va = s_dma_va ? map_region(g_bfm_mr_base, g_bfm_mr_size, "公開メモリ") : NULL;
    if (s_dma_va == NULL || s_mr_va == NULL) {
        bfm_port_fini();
        return -1;
    }
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
    if (s_bar0) bfm_port_bar0_unmap();
    if (s_device >= 0) close(s_device);
    if (s_group >= 0) close(s_group);
    if (s_container >= 0) close(s_container);   /* IOMMU の対応づけもここで外れる */
    s_device = s_group = s_container = -1;
    if (s_dma_va) munmap(s_dma_va, g_bfm_dma_size);
    if (s_mr_va) munmap(s_mr_va, g_bfm_mr_size);
    s_dma_va = s_mr_va = NULL;
    if (s_dma_latency_fd >= 0) close(s_dma_latency_fd);
    s_dma_latency_fd = -1;
}

uint32_t bfm_port_pci_read32(uint32_t off)
{
    uint32_t v = 0xffffffffu;
    if (pread(s_device, &v, 4, (off_t)(s_cfg_off + off)) != 4)
        bfm_port_log("port: 設定空間 0x%x を読めない (errno=%d)\n", off, errno);
    return v;
}

void bfm_port_pci_write32(uint32_t off, uint32_t val)
{
    if (pwrite(s_device, &val, 4, (off_t)(s_cfg_off + off)) != 4)
        bfm_port_log("port: 設定空間 0x%x へ書けない (errno=%d)\n", off, errno);
}

/* ---------------------------------------------------------------------
 * BAR0
 * ------------------------------------------------------------------- */

/* [off, off+len) を UC(sysfs の resource0、だめなら VFIO)で va に張る。 */
static void *bar_map_uc(void *va, uint64_t off, uint64_t len, int fixed)
{
    void *p = MAP_FAILED;
    const int flags = MAP_SHARED | (fixed ? MAP_FIXED : 0);
    if (s_bar0_sysfs) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", g_bfm_linux_pci_bdf);
        const int fd = open(path, O_RDWR | O_SYNC);
        if (fd >= 0) {
            p = mmap(va, len, PROT_READ | PROT_WRITE, flags, fd, (off_t)off);
            close(fd);
        }
    } else {
        p = mmap(va, len, PROT_READ | PROT_WRITE, flags, s_device, (off_t)(s_bar0_vfio_off + off));
    }
    return p;
}

volatile uint8_t *bfm_port_bar0_map(uint64_t bar_phys, uint64_t bar_size)
{
    (void)bar_phys;
    if (bar_size != 0 && bar_size != s_bar0_size)
        bfm_port_log("port: 注意: BAR0 の大きさが VFIO(0x%llx)と設定空間(0x%llx)で違う\n",
                     (unsigned long long)s_bar0_size, (unsigned long long)bar_size);
    s_bar0_sysfs = 1;
    void *p = bar_map_uc(NULL, 0, s_bar0_size, 0);
    if (p == MAP_FAILED) {
        bfm_port_log("port: sysfs の resource0 が張れない。VFIO から張る(BlueFlame は使えない)\n");
        s_bar0_sysfs = 0;
        p = bar_map_uc(NULL, 0, s_bar0_size, 0);
    }
    if (p == MAP_FAILED) return NULL;
    s_bar0 = (uint8_t *)p;
    return s_bar0;
}

void bfm_port_bar0_unmap(void)
{
    if (s_bar0) munmap(s_bar0, s_bar0_size);
    s_bar0 = NULL;
    s_nseg = 0;
}

/* UC で張った区間を表に足す(表が一杯なら足さない。その区間は WC に張り替えられなくなるだけ)。 */
static void seg_add(uint64_t off, uint64_t len)
{
    if (s_nseg >= SEG_MAX) return;
    s_seg[s_nseg].off = off;
    s_seg[s_nseg].len = len;
    s_nseg++;
}

/*=================================================================
 * BAR0 の [off, off+len) を同じ仮想アドレスのまま write-combining に張り替える。
 * そのページを含む mmap の前と後を張り直して、元の mmap をそのページだけに縮め、
 * そのページを外してから resource0_wc で張る。失敗したら UC に戻して NULL。
 * ===============================================================*/
volatile uint8_t *bfm_port_bar0_wc(uint64_t off, uint64_t len)
{
    if (!s_bar0 || !s_bar0_sysfs || off + len > s_bar0_size) return NULL;
    if (s_nseg == 0) {
        s_seg[0].off = 0;
        s_seg[0].len = s_bar0_size;
        s_nseg = 1;
    }
    int k = -1;
    for (int i = 0; i < s_nseg; i++)
        if (off >= s_seg[i].off && off + len <= s_seg[i].off + s_seg[i].len) k = i;
    if (k < 0 || s_nseg + 2 > SEG_MAX) return NULL;
    uint8_t *va = s_bar0 + off;
    const uint64_t s0 = s_seg[k].off, s1 = s_seg[k].off + s_seg[k].len;
    if (off > s0 && bar_map_uc(s_bar0 + s0, s0, off - s0, 1) != (void *)(s_bar0 + s0)) return NULL;
    if (off + len < s1 && bar_map_uc(va + len, off + len, s1 - off - len, 1) != (void *)(va + len)) return NULL;
    /* ここまでで前後は別の mmap になった。表もそのとおりに分ける
     * (前後の張り直しに失敗したときは、全部 UC のままなので表は変えない)。 */
    s_seg[k] = s_seg[--s_nseg];
    if (off > s0) { s_seg[s_nseg].off = s0; s_seg[s_nseg].len = off - s0; s_nseg++; }
    if (off + len < s1) { s_seg[s_nseg].off = off + len; s_seg[s_nseg].len = s1 - off - len; s_nseg++; }
    if (munmap(va, len) != 0) {
        seg_add(off, len);                                   /* 外せなかった = UC のまま残っている */
        return NULL;
    }
    char path[128];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0_wc", g_bfm_linux_pci_bdf);
    const int fd = open(path, O_RDWR | O_SYNC);
    void *p = MAP_FAILED;
    if (fd >= 0) {
        p = mmap(va, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)off);
        close(fd);
    }
    if (p == (void *)va) return va;
    bfm_port_log("port: %s が張れない (errno=%d)。UC に戻す\n", path, errno);
    if (bar_map_uc(va, off, len, 1) == (void *)va) seg_add(off, len);
    return NULL;
}

void bfm_port_bar0_wc_release(uint64_t off, uint64_t len)
{
    if (!s_bar0 || off + len > s_bar0_size) return;
    uint8_t *va = s_bar0 + off;
    if (munmap(va, len) == 0 && bar_map_uc(va, off, len, 1) == (void *)va) seg_add(off, len);
}
