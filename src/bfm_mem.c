/* DMA 領域(g_bfm_dma_base / g_bfm_dma_size)からの静的な切り出し。
 *
 * 返すことはしない(bfm_init() のたびに先頭から切り直す)。接続ごとの資源は
 * bfm_init() のときに BFM_MAX_CONN 本ぶんを先に切り出しておく。 */
#include "bfm_internal.h"

#include <string.h>

static uint64_t s_next;

void bfm_mem_reset(void)
{
    s_next = 0;
}

/*=================================================================
 * size バイトを align の境界で切り出し、0 で埋めて返す。
 * 足りなければ NULL(必要な大きさを表示する)。
 * ===============================================================*/
void *bfm_mem_alloc(uint64_t size, uint64_t align)
{
    const uint64_t base = g_bfm_dma_base;
    uint64_t at = (base + s_next + align - 1u) & ~(align - 1u);
    if (at + size > base + g_bfm_dma_size) {
        BFM_LOG("bfm: DMA 領域が足りない(%llu バイト使用済み、さらに %llu バイト要る。"
                "g_bfm_dma_size = 0x%llx)\n", (unsigned long long)s_next,
                (unsigned long long)size, (unsigned long long)g_bfm_dma_size);
        return NULL;
    }
    s_next = at + size - base;
    void *p = (void *)(uintptr_t)at;
    memset(p, 0, (size_t)size);
    bfm_port_cache_flush(p, (size_t)size);
    return p;
}

uint64_t bfm_mem_used(void)
{
    return s_next;
}
