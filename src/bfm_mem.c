/* DMA 領域(g_bfm_dma_base / g_bfm_dma_size)からの静的な切り出し。
 *
 * 返すことはしない(bfm_init() のたびに先頭から切り直す)。接続ごとの資源は
 * bfm_init() のときに g_bfm_max_conn 本ぶんを先に切り出しておく。 */
#include "bfm_internal.h"

#include <string.h>

static uint64_t s_next;

void bfm_mem_reset(void)
{
    s_next = 0;
}

/*=================================================================
 * size バイトを align の境界で切り出し、0 で埋めて返す。
 * align は 2 の冪(0 は 1 とみなす)。
 * 足りなければ NULL(足りない量を表示する)。
 * ===============================================================*/
void *bfm_mem_alloc(uint64_t size, uint64_t align)
{
    if (align == 0) align = 1u;
    if ((align & (align - 1u)) != 0) {
        BFM_LOG("bfm: bfm_mem_alloc: align = 0x%llx が 2 の冪ではない\n", (unsigned long long)align);
        return NULL;
    }
    const uint64_t base = g_bfm_dma_base;
    const uint64_t end = base + g_bfm_dma_size;
    const uint64_t at = (base + s_next + align - 1u) & ~(align - 1u);
    /* 足し算の桁あふれを避けて、残りの大きさと比べる */
    if (at < base || at > end || size > end - at) {
        const uint64_t need = (at < base || at > end) ? size : size - (end - at);
        BFM_LOG("bfm: DMA 領域が足りない(%llu バイト使用済み。%llu バイトを切り出すのに %llu バイト不足。"
                "g_bfm_dma_size = 0x%llx)\n", (unsigned long long)s_next, (unsigned long long)size,
                (unsigned long long)need, (unsigned long long)g_bfm_dma_size);
        return NULL;
    }
    s_next = at + size - base;
    void *p = (void *)(uintptr_t)at;
    memset(p, 0, (size_t)size);
    return p;
}

uint64_t bfm_mem_used(void)
{
    return s_next;
}
