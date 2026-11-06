/* bfmem の試験プログラム(Linux + VFIO)。
 *
 *   bfmem_test [オプション] server                 受け側。公開メモリを決まった模様で埋めて待つ
 *   bfmem_test [オプション] client <IP> <MAC>      掛け側。相手の公開メモリへ一通りの操作をして確かめ、速さを測る
 *   bfmem_test [オプション] info                   立ち上げて能力を表示するだけ
 *
 * 相手は同じプログラムの server か、Linux の tools/bfm_peer.c(カーネルの mlx5 で動く)。
 *
 * 公開メモリの取り決め(server と bfm_peer で同じ):
 *   [0, 64KiB)          データ領域。(i * 7 + 3) & 0xff で埋める
 *   [64KiB, 64KiB+4KiB) 8 バイトの atomic 語 x 512(0 で埋める)
 * client はデータ領域の中身を自分でも持っておき(模型)、最後に読み戻して突き合わせ、
 * FNV-1a 64 の hash を表示する。server は Ctrl-\(SIGQUIT)か切断のときに自分のメモリの
 * hash を表示するので、両方の値が同じことを見る(両側が同じ間違い方をしても気付けるように)。 */
#define _GNU_SOURCE
#include "bfm.h"
#include "../src/bfm_internal.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DATA_BYTES   65536u
#define ATOM_OFF     65536u
#define ATOM_WORDS   512u
#define AREA_BYTES   (ATOM_OFF + ATOM_WORDS * 8u)

static volatile sig_atomic_t g_stop, g_dump;
static int g_server;                 /* server のときだけ SIGQUIT で hash を出す */
static uint8_t g_model[DATA_BYTES];

static void on_sig(int s)
{
    if (s == SIGQUIT && g_server) g_dump = 1;
    else g_stop = 1;                 /* server 以外では SIGQUIT も止める合図(後片付けをしてから終わる)*/
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint8_t pattern(uint32_t i)
{
    return (uint8_t)((i * 7u + 3u) & 0xffu);
}

static uint64_t fnv1a(const volatile uint8_t *p, uint32_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int parse_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static int parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return 0;
}

static void usage(void)
{
    printf("使い方: bfmem_test [-d BDF] [-i 自分のIP] [-p ポート] [-m 経路MTU 1-5] [-M ポートのMTU] [-n 回数] [-B] [-S] [-C] [-x] [-v]\n"
           "                   server | client <相手のIP> <相手のMAC> | bench <相手のIP> <相手のMAC> | info\n"
           "  bench: write / read を 4B〜256KiB、多重度 1・4・8・16 で -n 回(既定 100000)ずつ流し、\n"
           "         IOPS・MB/s・平均遅延を出す\n"
           "  -B  BlueFlame を使わない(8 バイトのドアベルだけ。比べるため)\n"
           "  -S  read / atomic の応答を CQE に書かせない(scatter to CQE を使わない)\n"
           "  -C  CPU を深い省電力状態に入れない設定(g_bfm_cpu_awake)を切る\n"
           "  -x  最後に公開メモリの範囲の外を読み、相手の NIC が断ることを確かめる(接続はエラーになる)\n");
}

/* ---------------------------------------------------------------------
 * server
 * ------------------------------------------------------------------- */

static void fill_area(void)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)g_bfm_mr_base;
    for (uint32_t i = 0; i < DATA_BYTES; i++) m[i] = pattern(i);
    for (uint32_t i = 0; i < ATOM_WORDS * 8u; i++) m[ATOM_OFF + i] = 0;
}

static void dump_area(const char *why)
{
    const volatile uint8_t *m = (const volatile uint8_t *)(uintptr_t)g_bfm_mr_base;
    printf("server: [%s] データ領域の hash = %016llx\n", why, (unsigned long long)fnv1a(m, DATA_BYTES));
    for (unsigned w = 0; w < 3u; w++) {
        const volatile uint8_t *p = m + ATOM_OFF + w * 8u;
        uint64_t le = 0;
        for (int i = 7; i >= 0; i--) le = (le << 8) | p[i];
        printf("server:   atomic 語 %u: %02x %02x %02x %02x %02x %02x %02x %02x(LE として %llu)\n", w,
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], (unsigned long long)le);
    }
}

static int run_server(uint16_t port)
{
    if (g_bfm_mr_size < AREA_BYTES) {
        printf("server: 公開メモリが小さすぎる\n");
        return 1;
    }
    while (!g_stop) {
        fill_area();
        printf("server: ポート %u で接続を待つ(Ctrl-C で終わる、Ctrl-\\ で hash)\n", port);
        bfm_conn_t *c = NULL;
        while (!g_stop && c == NULL) {
            c = bfm_accept(port, 500u);
            if (g_dump) { g_dump = 0; dump_area("要求"); }
        }
        if (c == NULL) break;
        while (!g_stop && bfm_is_up(c)) {
            (void)bfm_poll();
            if (g_dump) { g_dump = 0; dump_area("要求"); }
            usleep(1000);
        }
        dump_area("切断");
        (void)bfm_disconnect(c);
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * client
 * ------------------------------------------------------------------- */

static int g_ng;

static void check(const char *what, int ok)
{
    printf("client: %-58s %s\n", what, ok ? "OK" : "NG");
    if (!ok) g_ng++;
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t g_lat[200000];

typedef int (*op_fn)(bfm_conn_t *c, uint32_t i);
static uint8_t g_src[256];
static int op_write8(bfm_conn_t *c, uint32_t i) { return bfm_write(c, (i & 63u) * 64u, g_src, 8); }
static int op_write64(bfm_conn_t *c, uint32_t i) { return bfm_write(c, (i & 63u) * 64u, g_src, 64); }
static int op_read8(bfm_conn_t *c, uint32_t i) { uint8_t d[8]; return bfm_read(c, (i & 63u) * 64u, d, 8); }
static int op_read64(bfm_conn_t *c, uint32_t i) { uint8_t d[64]; return bfm_read(c, (i & 63u) * 64u, d, 64); }
static int op_faa(bfm_conn_t *c, uint32_t i) { uint64_t o; return bfm_faa(c, ATOM_OFF + 8u * 100u + (i & 63u) * 8u, 1, &o); }
static int op_cas(bfm_conn_t *c, uint32_t i) { uint64_t o; return bfm_cas(c, ATOM_OFF + 8u * 200u + (i & 63u) * 8u, 1, 2, &o); }

static void lat(bfm_conn_t *c, const char *name, op_fn fn, uint32_t n)
{
    if (n > sizeof(g_lat) / sizeof(g_lat[0])) n = sizeof(g_lat) / sizeof(g_lat[0]);
    for (uint32_t i = 0; i < n / 10u + 100u; i++) {
        if (fn(c, i) != BFM_OK) { g_ng++; return; }
    }
    uint64_t sum = 0;
    uint32_t m = 0;                  /* 測れた回数(止められたら n より少ない)*/
    for (; m < n && !g_stop; m++) {
        const uint64_t t0 = now_ns();
        if (fn(c, m) != BFM_OK) { g_ng++; return; }
        g_lat[m] = (uint32_t)(now_ns() - t0);
        sum += g_lat[m];
    }
    if (m == 0) return;
    n = m;
    qsort(g_lat, n, sizeof(g_lat[0]), cmp_u32);
    printf("client: 遅延 %-9s x%u: 中央値 %u ns / 平均 %llu / 最小 %u / p99 %u / p99.9 %u\n", name, n,
           g_lat[n / 2u], (unsigned long long)(sum / n), g_lat[0], g_lat[(uint64_t)n * 99u / 100u],
           g_lat[(uint64_t)n * 999u / 1000u]);
}

static int run_client(uint32_t ip, const uint8_t mac[6], uint16_t port, uint32_t iters, int oob)
{
    bfm_conn_t *c = bfm_connect(ip, mac, port, 0);
    if (c == NULL) return 1;
    printf("client: 相手の公開メモリ %llu バイト、BlueFlame %s、inline %u バイトまで\n",
           (unsigned long long)bfm_remote_size(c), bfm_bf_active(c) ? "あり" : "なし", bfm_inline_max());
    if (bfm_remote_size(c) < AREA_BYTES) {
        printf("client: 相手の公開メモリが小さすぎる(%u バイト要る)\n", AREA_BYTES);
        bfm_disconnect(c);
        return 1;
    }
    for (uint32_t i = 0; i < DATA_BYTES; i++) g_model[i] = pattern(i);
    static uint8_t buf[DATA_BYTES];
    int rc;

    /* [1] 初期の中身を、いろいろな長さ・半端な位置で読む。 */
    {
        int ok = 1;
        for (uint32_t len = 1; len <= 300u && ok; len += (len < 40u) ? 1u : 37u) {
            const uint32_t off = (len * 977u) % (DATA_BYTES - len);
            memset(buf, 0xee, len + 8u);
            if ((rc = bfm_read(c, off, buf, len)) != BFM_OK) { printf("client: read: %s\n", bfm_strerror(rc)); ok = 0; break; }
            if (memcmp(buf, &g_model[off], len) != 0 || buf[len] != 0xee) ok = 0;
        }
        rc = bfm_read(c, 0, buf, DATA_BYTES);
        check("[1] read 1〜300B(半端な位置)と 64KiB まとめて", ok && rc == BFM_OK && memcmp(buf, g_model, DATA_BYTES) == 0);
    }
    /* [2] write: inline に収まる長さと収まらない長さ。書いた後に読み戻す。 */
    {
        int ok = 1;
        uint32_t seed = 12345u;
        for (uint32_t k = 0; k < 400u && ok && !g_stop; k++) {
            seed = seed * 1103515245u + 12345u;
            const uint32_t len = (k < 300u) ? 1u + (k % bfm_inline_max()) : 221u + (seed % 8000u);
            const uint32_t off = (seed >> 8) % (DATA_BYTES - len);
            for (uint32_t j = 0; j < len; j++) buf[j] = (uint8_t)(seed >> (j & 15u)) ^ (uint8_t)j;
            if (bfm_write(c, off, buf, len) != BFM_OK) { ok = 0; break; }
            memcpy(&g_model[off], buf, len);
            if (bfm_read(c, off, buf, len) != BFM_OK || memcmp(buf, &g_model[off], len) != 0) ok = 0;
        }
        check("[2] write 1〜220B(inline)と 221〜8220B、毎回読み戻す", ok);
    }
    /* [3] 完了を待たない write を続けて出し、まとめて待つ。 */
    {
        int ok = 1;
        for (uint32_t k = 0; k < 5000u && ok; k++) {
            const uint32_t len = 1u + (k % 32u);
            const uint32_t off = (k * 131u) % (DATA_BYTES - len);
            for (uint32_t j = 0; j < len; j++) buf[j] = (uint8_t)(k + j * 3u);
            if (bfm_write_post(c, off, buf, len) != BFM_OK) ok = 0;
            memcpy(&g_model[off], buf, len);
        }
        if (bfm_flush(c) != BFM_OK) ok = 0;
        if (bfm_read(c, 0, buf, DATA_BYTES) != BFM_OK || memcmp(buf, g_model, DATA_BYTES) != 0) ok = 0;
        check("[3] write_post 1〜32B x5000 + flush の後に全体を読み戻す", ok);
    }
    /* [4] atomic。語 0 へ FAA +1 x1000(元の値が 0,1,2,...)、語 1 を CAS で 0 -> 1000、
     *     外れる比較値の CAS で値が変わらないこと、FAA が 32bit で折り返さないこと。 */
    {
        int ok = 1;
        uint64_t old = 0;
        for (uint64_t i = 0; i < 1000u && ok; i++)
            if (bfm_faa(c, ATOM_OFF, 1, &old) != BFM_OK || old != i) ok = 0;
        if (ok && (bfm_faa(c, ATOM_OFF, 0x100000000ull, &old) != BFM_OK || old != 1000u)) ok = 0;
        if (ok && (bfm_faa(c, ATOM_OFF, 0, &old) != BFM_OK || old != 0x100000000ull + 1000u)) ok = 0;
        check("[4] FAA +1 x1000、+2^32(元の値の並び)", ok);
        ok = 1;
        for (uint64_t i = 0; i < 1000u && ok; i++)
            if (bfm_cas(c, ATOM_OFF + 8u, i, i + 1u, &old) != BFM_OK || old != i) ok = 0;
        if (ok && (bfm_cas(c, ATOM_OFF + 8u, 7, 99, &old) != BFM_OK || old != 1000u)) ok = 0;
        if (ok && (bfm_cas(c, ATOM_OFF + 8u, 1000, 1000, &old) != BFM_OK || old != 1000u)) ok = 0;
        check("[4] CAS 0->1000(毎回成功)、外れる比較値で変わらない", ok);
        uint8_t raw[8];
        if (bfm_read(c, ATOM_OFF + 8u, raw, 8) == BFM_OK) {
            const int le = (raw[0] == 0xe8 && raw[1] == 0x03);
            const int be = (raw[7] == 0xe8 && raw[6] == 0x03);
            printf("client: [4] 相手のメモリの語 1(値 1000)の並び: %02x %02x %02x %02x %02x %02x %02x %02x = %s\n",
                   raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                   le ? "リトルエンディアン(ホスト順)" : be ? "ビッグエンディアン" : "?");
            if (!le && !be) g_ng++;
        }
        printf("client: [4] この HCA は atomic の元の値を %s で返す設定\n",
               g_bfm.atomic_host_order ? "ホスト順" : "ビッグエンディアン");
    }
    /* [5] 全体を読み戻して模型と突き合わせる。 */
    {
        rc = bfm_read(c, 0, buf, DATA_BYTES);
        check("[5] データ領域 64KiB を読み戻して模型と一致", rc == BFM_OK && memcmp(buf, g_model, DATA_BYTES) == 0);
    }
    /* [6] 遅延(1 個ずつ待つ)と、完了を待たない write の速さ。 */
    if (!g_stop && g_ng == 0) {
        for (unsigned i = 0; i < sizeof(g_src); i++) g_src[i] = (uint8_t)(0x5a ^ i);
        lat(c, "write 8B", op_write8, iters);
        lat(c, "write 64B", op_write64, iters);
        lat(c, "read 8B", op_read8, iters);
        lat(c, "read 64B", op_read64, iters);
        lat(c, "FAA", op_faa, iters);
        lat(c, "CAS", op_cas, iters);
        const uint32_t n = 200000u;
        const uint64_t t0 = now_ns();
        for (uint32_t i = 0; i < n; i++)
            if (bfm_write_post(c, (i & 63u) * 64u, g_src, 8) != BFM_OK) { g_ng++; break; }
        if (bfm_flush(c) != BFM_OK) g_ng++;
        const uint64_t dt = now_ns() - t0;
        printf("client: write_post 8B x%u: %.2f Mops\n", n, (double)n * 1000.0 / (double)dt);
        /* 測る間に書いたもの: 64 か所(64 バイトおき)が全部 g_src の先頭 64 バイトになっている。 */
        for (uint32_t k = 0; k < 64u; k++) memcpy(&g_model[k * 64u], g_src, 64);
        rc = bfm_read(c, 0, buf, DATA_BYTES);
        check("[6] 測った後のデータ領域が模型と一致", rc == BFM_OK && memcmp(buf, g_model, DATA_BYTES) == 0);
    }
    printf("client: 模型の hash = %016llx(相手の server / bfm_peer が切断のときに出す値と同じはず)\n",
           (unsigned long long)fnv1a(g_model, DATA_BYTES));
    /* [7] 範囲の外。相手の NIC が REMOTE_ACCESS で断ること(相手の MKey が範囲を持っていること)。 */
    if (oob && !g_stop) {
        struct bfm_conn *ci = (struct bfm_conn *)c;
        const uint64_t real = ci->rlen;
        ci->rlen = real + 4096u;                  /* 手元の範囲検査だけをすり抜ける */
        uint8_t d[8];
        rc = bfm_read(c, real, d, 8);
        ci->rlen = real;
        check("[7] 公開メモリの外を読むと相手が断る(REMOTE_ACCESS)", rc == BFM_E_REMOTE);
    }
    printf("client: 結果 %s(NG %d 件)\n", g_ng ? "NG" : "OK", g_ng);
    (void)bfm_disconnect(c);
    return g_ng ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * bench: 多重度(同時に出しておく数)ごとのスループット・IOPS・遅延
 * ------------------------------------------------------------------- */

#define BENCH_QD_MAX   16u
#define BENCH_SIZE_MAX (256u * 1024u)

static double g_tsc_per_ns;

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void tsc_calibrate(void)
{
    const uint64_t n0 = now_ns(), c0 = rdtsc();
    usleep(200000);
    g_tsc_per_ns = (double)(rdtsc() - c0) / (double)(now_ns() - n0);
}

/*=================================================================
 * op(0 = write、1 = read)を size バイトで n 回、常に qd 個を出しておく形で流す。
 * 1 回ごとに完了通知を求め(完了は出した順に来る)、出してから完了を拾うまでを遅延とする。
 * 相手のアドレスは size ずつずらし、相手の公開メモリの中で回す。
 * ===============================================================*/
static int bench_one(bfm_conn_t *c, int is_read, uint32_t size, uint32_t qd, uint32_t n)
{
    struct bfm_conn *ci = (struct bfm_conn *)c;
    static uint64_t ts[BENCH_QD_MAX];
    const uint64_t slots = (bfm_remote_size(c) / size) < 1024u ? bfm_remote_size(c) / size : 1024u;
    if (slots == 0 || qd == 0 || qd > BENCH_QD_MAX || size > BENCH_SIZE_MAX) {
        printf("bench: %u バイト x 多重度 %u は流せない(相手の公開メモリ %llu バイト)\n", size, qd,
               (unsigned long long)bfm_remote_size(c));
        return -1;
    }
    uint32_t posted = 0, done = 0;
    uint64_t lat_sum = 0;
    const uint64_t t0 = rdtsc();
    while (done < n) {
        while (posted - done < qd && posted < n) {
            const uint32_t k = posted % qd;
            ts[k] = rdtsc();
            const int rc = bfm_ops_post_rw(ci, is_read, (posted % slots) * size, k * size, g_src, size);
            if (rc != BFM_OK) { printf("bench: 出せない: %s\n", bfm_strerror(rc)); return -1; }
            posted++;
        }
        const int r = bfm_ops_poll1(ci);
        if (r < 0) { printf("bench: エラー完了: %s\n", bfm_strerror(r)); return -1; }
        if (r > 0) {
            lat_sum += rdtsc() - ts[done % qd];
            done++;
        }
        if (g_stop) return -1;
    }
    const double sec = (double)(rdtsc() - t0) / g_tsc_per_ns / 1e9;
    const double iops = (double)n / sec;
    printf("bench: %-5s %7u %3u %12.0f %10.1f %10.2f\n", is_read ? "read" : "write", size, qd,
           iops, iops * size / 1e6, (double)lat_sum / g_tsc_per_ns / n / 1000.0);
    fflush(stdout);
    return 0;
}

static int run_bench(uint32_t ip, const uint8_t mac[6], uint16_t port, uint32_t n)
{
    static const uint32_t sizes[] = { 4, 8, 32, 64, 128, 256, 512, 8192, 65536, 262144 };
    static const uint32_t qds[] = { 1, 4, 8, 16 };
    bfm_conn_t *c = bfm_connect(ip, mac, port, 0);
    if (c == NULL) return 1;
    tsc_calibrate();
    for (unsigned i = 0; i < sizeof(g_src); i++) g_src[i] = (uint8_t)(0x5a ^ i);
    {
        /* 埋め込みでない write は中継バッファから送るので、送る中身を置いておく */
        struct bfm_conn *ci = (struct bfm_conn *)c;
        for (uint32_t i = 0; i < ci->stage_size; i++) ci->stage[i] = (uint8_t)(0x5a ^ i);
        bfm_port_cache_flush(ci->stage, ci->stage_size);
    }
    printf("bench: 各 %u 回。相手の公開メモリ %llu バイト、BlueFlame %s、inline %u バイトまで、MTU %u\n", n,
           (unsigned long long)bfm_remote_size(c), bfm_bf_active(c) ? "あり" : "なし", bfm_inline_max(),
           128u << ((struct bfm_conn *)c)->path_mtu);
    printf("bench: 操作  バイト 多重度         IOPS       MB/s   遅延(us)\n");
    int ng = 0;
    for (int is_read = 0; is_read < 2 && !ng; is_read++)
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]) && !ng; s++)
            for (unsigned q = 0; q < sizeof(qds) / sizeof(qds[0]) && !ng; q++)
                if (bench_one(c, is_read, sizes[s], qds[q], n) != 0) ng = 1;
    (void)bfm_disconnect(c);
    return ng;
}

int main(int argc, char **argv)
{
    uint16_t port = 0;
    uint32_t iters = 0;
    int oob = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d:i:p:m:M:n:BSCxvh")) != -1) {
        switch (opt) {
        case 'd': g_bfm_linux_pci_bdf = optarg; break;
        case 'i': if (parse_ip(optarg, &g_bfm_local_ip) != 0) { usage(); return 2; } break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 'm': g_bfm_path_mtu = (uint8_t)atoi(optarg); break;
        case 'M': g_bfm_port_mtu = (uint16_t)atoi(optarg); break;
        case 'n': iters = (uint32_t)atoi(optarg); break;
        case 'B': g_bfm_bf_enable = 0; break;
        case 'S': g_bfm_scatter_cqe = 0; break;
        case 'C': g_bfm_cpu_awake = 0; break;
        case 'x': oob = 1; break;
        case 'v': g_bfm_verbose = 1; break;
        default: usage(); return 2;
        }
    }
    if (optind >= argc) { usage(); return 2; }
    const char *mode = argv[optind];
    if (port == 0) port = g_bfm_cm_port;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGQUIT, on_sig);

    const int bench = (strcmp(mode, "bench") == 0);
    g_server = (strcmp(mode, "server") == 0);
    if (bench) g_bfm_stage_size = BENCH_QD_MAX * BENCH_SIZE_MAX;   /* 多重度ぶんの着地点 / 送り元 */
    if (bfm_init() != BFM_OK) return 1;
    int ret = 0;
    if (strcmp(mode, "server") == 0) {
        ret = run_server(port);
    } else if ((strcmp(mode, "client") == 0 || bench) && optind + 2 < argc) {
        uint32_t ip;
        uint8_t mac[6];
        if (parse_ip(argv[optind + 1], &ip) != 0 || parse_mac(argv[optind + 2], mac) != 0) {
            usage();
            ret = 2;
        } else if (bench) {
            ret = run_bench(ip, mac, port, iters ? iters : 100000u);
        } else {
            ret = run_client(ip, mac, port, iters ? iters : 20000u, oob);
        }
    } else if (strcmp(mode, "info") == 0) {
        printf("info: %s bf=%u bf_buf=%u atomic ops=0x%x size_qp=0x%x host_order=%u mode=%u ts_format=%u "
               "read/atomic req/res=%u/%u FW ページ %u\n", g_bfm.name, g_bfm.bf_cap, g_bfm.bf_buf_size,
               g_bfm.atomic_ops, g_bfm.atomic_size_qp, g_bfm.atomic_host_order, g_bfm.atomic_mode,
               g_bfm.qp_ts_format, 1u << g_bfm.log_max_ra_req_qp, 1u << g_bfm.log_max_ra_res_qp,
               g_bfm.fw_pages_used);
    } else {
        usage();
        ret = 2;
    }
    bfm_shutdown();
    return ret;
}
