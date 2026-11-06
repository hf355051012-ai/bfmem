/* make vxcheck 用の仮のヘッダ。 */
typedef enum { INSTRUCTION_CACHE, DATA_CACHE } CACHE_TYPE;
STATUS cacheFlush(CACHE_TYPE t, void *p, size_t n);
STATUS cacheInvalidate(CACHE_TYPE t, void *p, size_t n);
