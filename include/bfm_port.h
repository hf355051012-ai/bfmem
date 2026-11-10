#ifndef BFM_PORT_H
#define BFM_PORT_H

/* =====================================================================
 * 移植層。プラットフォームごとに port/<名前>/ で実装する関数と、
 * バリアとバイト順の道具。
 *
 *   port/linux_vfio/  x86-64 Linux + VFIO(試験環境)
 *   port/vxworks/     x86-64 VxWorks(論理アドレス = 物理アドレス)
 *
 * CPU は x86-64(Intel)だけ。次を前提にしている:
 *   - リトルエンディアン
 *   - DMA はキャッシュと一貫している(キャッシュの flush / invalidate は要らない)
 *   - TSO(書き込みどうしの順序は保たれる)、64bit の MMIO 書き込みは 1 回で出る
 *
 * アドレスの約束: DMA に使うアドレス(NIC に渡すアドレス)は CPU のポインタの値
 * そのもの。VxWorks は論理 = 物理なので自然に成り立ち、Linux はポート層が
 * 仮想アドレス = IOVA になるよう張る。
 * ===================================================================== */

#if !defined(__x86_64__)
#error "bfmem は x86-64 専用"
#endif

#include <stdint.h>
#include <stddef.h>

/* ---- ポート層が実装する関数 ----
 * port はポートの番号(0〜BFM_MAX_PORT-1)。ConnectX はポートごとに別の PCIe の function なので、
 * 設定空間と BAR0 はポートごとにある。 */

/* g_bfm_port_conn[] が 0 でないポートの装置を使えるようにし(Linux: VFIO で掴む)、DMA 領域と
 * 公開メモリ(g_bfm_dma_* / g_bfm_mr_*)を CPU と NIC の両方から同じアドレスで触れるようにする。
 * 0 = 成功。 */
int  bfm_port_init(void);
/* bfm_port_init() で取ったものを返す。呼ぶ前に全部のポートの Bus Master は落としてある。 */
void bfm_port_fini(void);

/* ポート port の PCIe の設定空間の 32bit 読み書き(off は 4 の倍数)。 */
uint32_t bfm_port_pci_read32(unsigned port, uint32_t off);
void     bfm_port_pci_write32(unsigned port, uint32_t off, uint32_t val);

/* ポート port の BAR0 を UC(キャッシュしない)で CPU から触れるようにして先頭を返す。
 * bar_phys は設定空間から読んだ BAR0 の値(Linux では使わない)。NULL = 失敗。 */
volatile uint8_t *bfm_port_bar0_map(unsigned port, uint64_t bar_phys, uint64_t bar_size);
void              bfm_port_bar0_unmap(unsigned port);

/* ポート port の BAR0 の [off, off+len)(UAR の 1 ページ)を write-combining で触れるようにして
 * 先頭を返す。BlueFlame の書き込み先。NULL = WC は使えない(BlueFlame を使わない)。
 * Linux は UC の窓のそのページを同じ仮想アドレスのまま WC に張り替えるので、
 * 返った後は bfm_port_bar0_map() の窓のそのページも WC になっている。 */
volatile uint8_t *bfm_port_bar0_wc(unsigned port, uint64_t off, uint64_t len);
void              bfm_port_bar0_wc_release(unsigned port, uint64_t off, uint64_t len);

/* 単調に増える時刻(ナノ秒)。分解能は粗くてよい(時間切れの判定に使う)。 */
uint64_t bfm_port_time_ns(void);
/* 待つ(マイクロ秒)。 */
void     bfm_port_delay_us(uint32_t us);
/* 表示(printf と同じ書式)。 */
void     bfm_port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- バリア(x86-64)----
 * bfm_dma_wmb(): ホストメモリへの書き込み(WQE・doorbell record)を、この後の
 *                MMIO(ドアベル)より先に NIC から見えるようにする。x86 は TSO なので
 *                コンパイラ障壁だけでよい(rdma-core の udma_to_device_barrier() と同じ)。
 * bfm_wc_flush(): write-combining のバッファを吐き出させる(BlueFlame の後)。
 * bfm_mb():      読み書き全部の順序(コマンドキューなど冷たい経路)。
 * bfm_rmb():     CQE の owner を読んでから中身を読む(x86 では読み出しどうしは追い越さない)。 */
#define bfm_dma_wmb()  __asm__ volatile("" ::: "memory")
#define bfm_wc_flush() __asm__ volatile("sfence" ::: "memory")
#define bfm_mb()       __asm__ volatile("mfence" ::: "memory")
#define bfm_rmb()      __asm__ volatile("" ::: "memory")

/* ---- バイト順 ----
 * ConnectX の構造(コマンド・WQE・CQE)はすべてビッグエンディアン。CPU はリトルエンディアン。 */
#define bfm_cpu_to_be32(v) __builtin_bswap32(v)
#define bfm_cpu_to_be64(v) __builtin_bswap64(v)
#define bfm_be32_to_cpu(v) __builtin_bswap32(v)
#define bfm_be64_to_cpu(v) __builtin_bswap64(v)

/* バイト列としての読み書き(揃っていない位置でもよい)。 */
static inline uint16_t bfm_rd16be(const volatile void *p) {
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static inline uint32_t bfm_rd24be(const volatile void *p) {
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}
static inline uint32_t bfm_rd32be(const volatile void *p) {
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static inline uint64_t bfm_rd64be(const volatile void *p) {
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return ((uint64_t)bfm_rd32be(b) << 32) | bfm_rd32be(b + 4);
}
static inline void bfm_wr16be(volatile void *p, uint16_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}
static inline void bfm_wr24be(volatile void *p, uint32_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)v;
}
static inline void bfm_wr32be(volatile void *p, uint32_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}
static inline void bfm_wr64be(volatile void *p, uint64_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    bfm_wr32be(b, (uint32_t)(v >> 32));
    bfm_wr32be(b + 4, (uint32_t)v);
}

/* ---- MMIO ---- */
static inline uint32_t bfm_mmio_rd32be(const volatile uint8_t *base, uint64_t off) {
    return bfm_be32_to_cpu(*(const volatile uint32_t *)(const volatile void *)(base + off));
}
static inline void bfm_mmio_wr32be(volatile uint8_t *base, uint64_t off, uint32_t v) {
    *(volatile uint32_t *)(volatile void *)(base + off) = bfm_cpu_to_be32(v);
}
/* 8 バイトのドアベルは 1 回の 64bit 書き込みで出す(32bit 2 回に分けると PCIe 上で
 * 分かれうる)。v はメモリ上の並び(WQE の先頭 8 バイトをそのまま)。 */
static inline void bfm_mmio_wr64_raw(volatile void *dst, uint64_t v) {
    *(volatile uint64_t *)dst = v;
}

/* DMA に使うアドレス(論理 = 物理 / 仮想 = IOVA)。 */
static inline uint64_t bfm_dma_addr(const volatile void *p) { return (uint64_t)(uintptr_t)p; }

#endif /* BFM_PORT_H */
