/* bfmem の試験プログラム(Linux + VFIO)。
 *
 *   bfmem_test [オプション] server                 受け側。公開メモリを決まった模様で埋めて待つ
 *   bfmem_test [オプション] client <IP> [MAC]      掛け側。相手の公開メモリへ一通りの操作をして確かめ、速さを測る
 *   bfmem_test [オプション] info                   立ち上げて能力を表示するだけ
 *   bfmem_test [-t 本数] mt <IP> [MAC]             掛け側。複数のスレッド(コア)から別々の接続で同時に転送する
 *
 * MAC を省くと、相手の MAC は bfm_connect() が自動で取る(REQ を同報で送り、REP から取る)。
 *
 * 相手は別の機械で動かす同じプログラムの server(相手は常に bfmem)。
 *
 * 公開メモリの取り決め(server が埋める):
 *   [0, 64KiB)          データ領域。(i * 7 + 3) & 0xff で埋める
 *   [64KiB, 64KiB+4KiB) 8 バイトの atomic 語 x 512(0 で埋める)
 * client はデータ領域の中身を自分でも持っておき(模型)、最後に読み戻して突き合わせ、
 * FNV-1a 64 の hash を表示する。server は Ctrl-\(SIGQUIT)か切断のときに自分のメモリの
 * hash を表示するので、両方の値が同じことを見る(両側が同じ間違い方をしても気付けるように)。 */
#define _GNU_SOURCE
#include "bfm.h"
#include "../src/bfm_internal.h"

#include <pthread.h>
#include <sched.h>
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

static volatile sig_atomic_t g_stop; /* 止める合図(SIGINT / SIGTERM、server 以外では SIGQUIT も)*/
static volatile sig_atomic_t g_dump; /* hash を出す合図(server で SIGQUIT)*/
static int g_server;                 /* server のときだけ SIGQUIT で hash を出す */
static uint8_t g_model[DATA_BYTES];  /* 相手のデータ領域にあるはずの中身(client の模型)*/

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
           "                   [-t スレッド数] server | client <相手のIP> [相手のMAC] | bench <相手のIP> [相手のMAC] |\n"
           "                   mt <相手のIP> [相手のMAC] | info\n"
           "  相手のMAC を省くと自動で取る(REQ を同報で送り、REP に入った MAC を使う)\n"
           "  bench: write / read を 4B〜256KiB、多重度 1・4・8・16 で -n 回(既定 100000)ずつ流し、\n"
           "         IOPS・MB/s・平均遅延を出す\n"
           "  -B  BlueFlame を使わない(8 バイトのドアベルだけ。比べるため)\n"
           "  -S  read / atomic の応答を CQE に書かせない(scatter to CQE を使わない)\n"
           "  -C  CPU を深い省電力状態に入れない設定(g_bfm_cpu_awake)を切る\n"
           "  -x  最後に公開メモリの範囲の外を読み、相手の NIC が断ることを確かめる(接続はエラーになる)\n"
           "  mt: -t 本(既定 4)のスレッドが同時に別々の接続を張り、write / read / atomic / T10-DIF を確かめ、\n"
           "      8 バイトの write / read / FAA を -n 回(既定 100000)ずつ同時に打って合計の速さを出す\n");
}

/* ---------------------------------------------------------------------
 * 待ちを返す関数を最後まで進める(BFM_INPROGRESS の間、少し眠っては呼び直す)
 * ------------------------------------------------------------------- */

/* 呼び直すまでの合間(本番ではここでほかの仕事をする)。 */
static void idle(void)
{
    usleep(50);
}

static int do_init(void)
{
    int rc;
    while ((rc = bfm_init()) == BFM_INPROGRESS) idle();
    return rc;
}

static void do_shutdown(void)
{
    while (bfm_shutdown() == BFM_INPROGRESS) idle();
}

/* 接続する。失敗なら NULL(理由は bfmem が表示する)。 */
static bfm_conn_t *do_connect(uint32_t ip, const uint8_t mac[6], uint16_t port, uint32_t ms)
{
    bfm_conn_t *c = NULL;
    int rc;
    while ((rc = bfm_connect(&c, ip, mac, port, ms)) == BFM_INPROGRESS) idle();
    return (rc == BFM_OK) ? c : NULL;
}

static int do_disconnect(bfm_conn_t *c)
{
    int rc;
    while ((rc = bfm_disconnect(c)) == BFM_INPROGRESS) idle();
    return rc;
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
    /* 接続を同時にいくつでも(接続の枠の数まで)受ける。全部が切れたら hash を出して埋め直す。
     * 受け付けの途中の枠(pending)を 1 つ持ち、回るたびに bfm_accept() で進める。 */
    static bfm_conn_t *conns[BFM_MAX_CONN];
    bfm_conn_t *pending = NULL;
    unsigned n = 0;
    fill_area();
    printf("server: ポート %u で接続を待つ(Ctrl-C で終わる、Ctrl-\\ で hash)\n", port);
    while (!g_stop) {
        if (n < g_bfm.nconn) {                     /* 受け付けの途中の枠も含めて枠の数まで */
            const int rc = bfm_accept(&pending, port, 0);
            if (rc == BFM_OK) {
                conns[n++] = pending;
                pending = NULL;
            }
        }
        (void)bfm_poll();
        if (g_dump) { g_dump = 0; dump_area("要求"); }
        unsigned k = 0;
        int closed = 0;
        for (unsigned i = 0; i < n; i++) {
            if (bfm_is_up(conns[i])) {
                conns[k++] = conns[i];
            } else {
                (void)do_disconnect(conns[i]);
                closed = 1;
            }
        }
        n = k;
        if (closed && n == 0) {
            dump_area("切断");
            fill_area();
            printf("server: ポート %u で接続を待つ(Ctrl-C で終わる、Ctrl-\\ で hash)\n", port);
        }
        usleep(n == g_bfm.nconn ? 1000 : 50);
    }
    if (pending != NULL) (void)do_disconnect(pending);
    for (unsigned i = 0; i < n; i++) (void)do_disconnect(conns[i]);
    return 0;
}

/* ---------------------------------------------------------------------
 * client
 * ------------------------------------------------------------------- */

static int g_ng;                     /* client の検査で NG になった数 */

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

static uint32_t g_lat[200000];       /* 遅延を測った値(ns。1 回ずつ。並べて中央値などを出す)*/

typedef int (*op_fn)(bfm_conn_t *c, uint32_t i);
static uint8_t g_src[256];           /* 遅延を測るときに write する元のデータ */
static int op_write8(bfm_conn_t *c, uint32_t i) { return bfm_write(c, (i & 63u) * 64u, g_src, 8); }
static int op_write64(bfm_conn_t *c, uint32_t i) { return bfm_write(c, (i & 63u) * 64u, g_src, 64); }
static int op_read8(bfm_conn_t *c, uint32_t i) { uint8_t d[8]; return bfm_read(c, (i & 63u) * 64u, d, 8); }
static int op_read64(bfm_conn_t *c, uint32_t i) { uint8_t d[64]; return bfm_read(c, (i & 63u) * 64u, d, 64); }
static int op_faa(bfm_conn_t *c, uint32_t i) { uint64_t o; return bfm_faa(c, ATOM_OFF + 8u * 100u + (i & 63u) * 8u, 1, &o); }
static int op_cas(bfm_conn_t *c, uint32_t i) { uint64_t o; return bfm_cas(c, ATOM_OFF + 8u * 200u + (i & 63u) * 8u, 1, 2, &o); }
static int op_faa32(bfm_conn_t *c, uint32_t i) { uint32_t o; return bfm_faa32(c, ATOM_OFF + 8u * 300u + (i & 63u) * 4u, 1, &o); }
static int op_cas32(bfm_conn_t *c, uint32_t i) { uint32_t o; return bfm_cas32(c, ATOM_OFF + 8u * 400u + (i & 63u) * 4u, 1, 2, &o); }

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

static void run_dif(bfm_conn_t *c, uint32_t iters);

static int run_client(uint32_t ip, const uint8_t mac[6], uint16_t port, uint32_t iters, int oob)
{
    bfm_conn_t *c = do_connect(ip, mac, port, 0);
    if (c == NULL) return 1;
    printf("client: 相手の公開メモリ %llu バイト、BlueFlame %s、inline %u バイトまで\n",
           (unsigned long long)bfm_remote_size(c), bfm_bf_active(c) ? "あり" : "なし", bfm_inline_max());
    if (bfm_remote_size(c) < AREA_BYTES) {
        printf("client: 相手の公開メモリが小さすぎる(%u バイト要る)\n", AREA_BYTES);
        (void)do_disconnect(c);
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
        /* 相手の HCA も atomic をホスト順(リトルエンディアン)で書く設定のはず */
        uint64_t v = 0;
        check("[4] 相手のメモリの語 1 がホスト順の 1000",
              bfm_read(c, ATOM_OFF + 8u, &v, 8) == BFM_OK && v == 1000u);
    }
    /* [4b] 4 バイトの atomic(語 2 の前半 = +16 を FAA、後半 = +20 を CAS)。
     *     FAA +1 x1000、2^32 - 1000 を足して 0 に折り返すこと(隣へ桁が上がらない)、
     *     CAS 0 -> 1000、外れる比較値で変わらないこと、相手のメモリのバイト順。 */
    {
        int ok = 1;
        uint32_t old = 0;
        for (uint32_t i = 0; i < 1000u && ok; i++)
            if (bfm_faa32(c, ATOM_OFF + 16u, 1, &old) != BFM_OK || old != i) ok = 0;
        if (ok && (bfm_faa32(c, ATOM_OFF + 16u, 0xfffffc18u, &old) != BFM_OK || old != 1000u)) ok = 0;
        if (ok && (bfm_faa32(c, ATOM_OFF + 16u, 0x01020304u, &old) != BFM_OK || old != 0)) ok = 0;
        if (ok && (bfm_faa32(c, ATOM_OFF + 16u, 0, &old) != BFM_OK || old != 0x01020304u)) ok = 0;
        check("[4b] FAA32 +1 x1000、0 への折り返し(元の値の並び)", ok);
        ok = 1;
        for (uint32_t i = 0; i < 1000u && ok; i++)
            if (bfm_cas32(c, ATOM_OFF + 20u, i, i + 1u, &old) != BFM_OK || old != i) ok = 0;
        if (ok && (bfm_cas32(c, ATOM_OFF + 20u, 7, 99, &old) != BFM_OK || old != 1000u)) ok = 0;
        if (ok && (bfm_cas32(c, ATOM_OFF + 20u, 1000, 0x0a0b0c0du, &old) != BFM_OK || old != 1000u)) ok = 0;
        check("[4b] CAS32 0->1000(毎回成功)、外れる比較値で変わらない", ok);
        uint8_t b[8] = {0};
        uint64_t v = 0;
        rc = bfm_read(c, ATOM_OFF + 16u, b, 8);
        printf("client: 相手のメモリ +16..+23: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        check("[4b] 相手のメモリがホスト順(04 03 02 01 0d 0c 0b 0a)",
              rc == BFM_OK && b[0] == 4 && b[1] == 3 && b[2] == 2 && b[3] == 1 &&
              b[4] == 0x0d && b[5] == 0x0c && b[6] == 0x0b && b[7] == 0x0a);
        check("[4b] 隣の 8 バイトの語 1 が 1000 のまま",
              bfm_read(c, ATOM_OFF + 8u, &v, 8) == BFM_OK && v == 1000u);
    }
    /* [5] 全体を読み戻して模型と突き合わせる。 */
    {
        rc = bfm_read(c, 0, buf, DATA_BYTES);
        check("[5] データ領域 64KiB を読み戻して模型と一致", rc == BFM_OK && memcmp(buf, g_model, DATA_BYTES) == 0);
    }
    /* [5b] 公開メモリの最後(データ領域とは別の所)。最後の 64KiB と 8 バイトを write して読み戻す。
     *     はみ出す操作は手元で断る(-x と合わせて、相手の MKey がちょうど公開メモリを覆うことを見る)。 */
    {
        const uint64_t end = bfm_remote_size(c);
        static uint8_t big[DATA_BYTES];
        int ok = end >= AREA_BYTES + DATA_BYTES;
        for (uint32_t j = 0; j < DATA_BYTES && ok; j++) buf[j] = (uint8_t)(j * 7u + 3u);
        if (ok && (bfm_write(c, end - DATA_BYTES, buf, DATA_BYTES) != BFM_OK ||
                   bfm_read(c, end - DATA_BYTES, big, DATA_BYTES) != BFM_OK ||
                   memcmp(big, buf, DATA_BYTES) != 0)) ok = 0;
        uint64_t w = 0x0123456789abcdefull, r = 0;
        if (ok && (bfm_write(c, end - 8u, &w, 8) != BFM_OK || bfm_read(c, end - 8u, &r, 8) != BFM_OK || r != w)) ok = 0;
        check("[5b] 公開メモリの最後の 64KiB と 8 バイトを write して読み戻す", ok);
        check("[5b] 公開メモリをはみ出す操作は手元で断る(BFM_E_PARAM)",
              bfm_read(c, end - 4u, &r, 8) == BFM_E_PARAM && bfm_write(c, end, &w, 1) == BFM_E_PARAM);
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
        lat(c, "FAA32", op_faa32, iters);
        lat(c, "CAS32", op_cas32, iters);
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
    /* [8] T10-DIF(データ領域とは別の所を使う)。 */
    if (!g_stop) run_dif(c, iters);
    printf("client: 模型の hash = %016llx(相手の server が切断のときに出す値と同じはず)\n",
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
    (void)do_disconnect(c);
    return g_ng ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * [8] T10-DIF(bfm_dif_write / bfm_dif_read)
 *
 * 相手の公開メモリの DIF_OFF から(データ領域・atomic 語とは別の所)を使う。
 *   R1 = DIF_OFF          520 バイトおきの形(PI つき)
 *   R2 = DIF_OFF + 256KiB 512 バイトおきの形(PI なし)
 * ブロック数 DIF_N は、DIF 用の中継バッファの 1 面(64KiB = 126 ブロック)ずつ 3 つの分に分けて
 * 運ぶ数(2 面を交互に使うので、3 つ目の分で面 0 を使い回す)。
 * PI は試験の側で独立に作り(CRC はビットごとの計算)、ライブラリの結果と突き合わせる。
 * SW は常に、HW は bfm_dif_hw_supported() のときだけ試す。
 * ------------------------------------------------------------------- */

#define DIF_OFF   (1024u * 1024u)
#define DIF_R2    (DIF_OFF + 256u * 1024u)
#define DIF_N     300u
#define DIF_BS    (BFM_DIF_BLOCK + BFM_DIF_PI)
#define DIF_APP   0x1234u
#define DIF_REF   0x00100000u
#define DIF_APP2  0xbeefu
#define DIF_REF2  0x7000fff0u            /* 途中で 0x7001xxxx に繰り上がる */

static uint8_t g_dplain[DIF_N * BFM_DIF_BLOCK];  /* PI の無い元のデータ(512 バイト x DIF_N)*/
static uint8_t g_dpi[DIF_N * DIF_BS];            /* 相手のメモリにあるはずの PI 付きのデータ(520 バイト x DIF_N)*/
static uint8_t g_dexp[DIF_N * DIF_BS];           /* 読んだ結果として期待する中身 */
static uint8_t g_dout[DIF_N * DIF_BS];           /* bfm_dif_read() で読んだ先 */
static uint8_t g_draw[DIF_N * DIF_BS];           /* 相手のメモリを bfm_read() でそのまま読んだ先 */

/* CRC16 T10-DIF をビットごとに(ライブラリの表引きとは別の書き方で)計算する。 */
static uint16_t crc16_bitwise(const uint8_t *p, uint32_t n)
{
    uint16_t crc = 0;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = (uint16_t)((crc & 0x8000u) ? (((uint32_t)crc << 1) ^ 0x8bb7u) : ((uint32_t)crc << 1));
    }
    return crc;
}

/* plain(512 バイトおき)から、PI つき(520 バイトおき)の期待値を作る。 */
static void dif_make(const uint8_t *plain, uint8_t *out, uint16_t app, uint32_t ref)
{
    for (uint32_t i = 0; i < DIF_N; i++) {
        uint8_t *b = out + i * DIF_BS;
        memcpy(b, plain + i * BFM_DIF_BLOCK, BFM_DIF_BLOCK);
        const uint16_t g = crc16_bitwise(b, BFM_DIF_BLOCK);
        const uint32_t r = ref + i;
        b[512] = (uint8_t)(g >> 8); b[513] = (uint8_t)g;
        b[514] = (uint8_t)(app >> 8); b[515] = (uint8_t)app;
        b[516] = (uint8_t)(r >> 24); b[517] = (uint8_t)(r >> 16); b[518] = (uint8_t)(r >> 8); b[519] = (uint8_t)r;
    }
}

static const char *dif_what(uint8_t w)
{
    return w == BFM_DIF_CHK_GUARD ? "guard" : w == BFM_DIF_CHK_APP ? "app" : w == BFM_DIF_CHK_REF ? "ref" : "?";
}

/* 誤りが期待どおりか(BFM_E_DIF、ブロック、種類)を確かめて表示する。 */
static void dif_expect_err(const char *name, int rc, const bfm_dif_err_t *e, uint32_t block, uint8_t what)
{
    char s[160];
    if (rc == BFM_E_DIF) {
        snprintf(s, sizeof(s), "%s(%s ブロック %u、あるべき 0x%x / 実際 0x%x)", name, dif_what(e->what),
                 e->block, e->expected, e->actual);
    } else {
        snprintf(s, sizeof(s), "%s(rc=%d %s)", name, rc, bfm_strerror(rc));
    }
    check(s, rc == BFM_E_DIF && e->block == block && e->what == what);
}

static void dif_tests(bfm_conn_t *c, uint8_t mode)
{
    const char *m = (mode == BFM_DIF_HW) ? "HW" : "SW";
    char name[96];
    bfm_dif_t d;
    bfm_dif_err_t e;
    int rc;
    uint32_t seed = (mode == BFM_DIF_HW) ? 777u : 333u;
    for (uint32_t i = 0; i < sizeof(g_dplain); i++) {
        seed = seed * 1103515245u + 12345u;
        g_dplain[i] = (uint8_t)(seed >> 16);
    }
    memset(&d, 0, sizeof(d));
    d.mode = mode;
    d.check = BFM_DIF_CHK_ALL;
    d.app_tag = DIF_APP;
    d.ref_tag = DIF_REF;
    d.new_app_tag = DIF_APP2;
    d.new_ref_tag = DIF_REF2;
#define DIF_CHECK(fmt, cond) do { snprintf(name, sizeof(name), "[8] %s: " fmt, m); check(name, (cond)); } while (0)

    /* write INSERT: 512 -> 相手 R1 に 520(PI を作る) */
    d.op = BFM_DIF_INSERT;
    rc = bfm_dif_write(c, DIF_OFF, g_dplain, DIF_N, &d, &e);
    dif_make(g_dplain, g_dexp, DIF_APP, DIF_REF);
    DIF_CHECK("write INSERT(相手に PI を作る)", rc == BFM_OK && bfm_read(c, DIF_OFF, g_draw, DIF_N * DIF_BS) == BFM_OK &&
              memcmp(g_draw, g_dexp, DIF_N * DIF_BS) == 0);
    memcpy(g_dpi, g_dexp, sizeof(g_dpi));               /* 以下、相手の R1 = g_dpi */

    /* read CHECK: R1 の 520 を検査して 520 のまま */
    d.op = BFM_DIF_CHECK;
    memset(g_dout, 0, sizeof(g_dout));
    rc = bfm_dif_read(c, DIF_OFF, g_dout, DIF_N, &d, &e);
    DIF_CHECK("read CHECK(520 のまま)", rc == BFM_OK && memcmp(g_dout, g_dpi, DIF_N * DIF_BS) == 0);

    /* read CHECK_SWAP: tag を置き換えて 520 */
    d.op = BFM_DIF_CHECK_SWAP;
    rc = bfm_dif_read(c, DIF_OFF, g_dout, DIF_N, &d, &e);
    dif_make(g_dplain, g_dexp, DIF_APP2, DIF_REF2);
    DIF_CHECK("read CHECK_SWAP(app / ref を置き換え)", rc == BFM_OK && memcmp(g_dout, g_dexp, DIF_N * DIF_BS) == 0);

    /* read CHECK_DELETE: 520 -> 512 */
    d.op = BFM_DIF_CHECK_DELETE;
    memset(g_dout, 0, sizeof(g_dout));
    rc = bfm_dif_read(c, DIF_OFF, g_dout, DIF_N, &d, &e);
    DIF_CHECK("read CHECK_DELETE(PI を除く)", rc == BFM_OK && memcmp(g_dout, g_dplain, sizeof(g_dplain)) == 0);

    /* 次のデータ(手元で PI を作る)。write CHECK で R1 に 520 のまま */
    for (uint32_t i = 0; i < sizeof(g_dplain); i++) g_dplain[i] = (uint8_t)(g_dplain[i] * 5u + 1u);
    dif_make(g_dplain, g_dpi, DIF_APP, DIF_REF);
    d.op = BFM_DIF_CHECK;
    rc = bfm_dif_write(c, DIF_OFF, g_dpi, DIF_N, &d, &e);
    DIF_CHECK("write CHECK(520 のまま)", rc == BFM_OK && bfm_read(c, DIF_OFF, g_draw, DIF_N * DIF_BS) == BFM_OK &&
              memcmp(g_draw, g_dpi, DIF_N * DIF_BS) == 0);

    /* write CHECK_SWAP: 相手には置き換えた tag */
    d.op = BFM_DIF_CHECK_SWAP;
    rc = bfm_dif_write(c, DIF_OFF, g_dpi, DIF_N, &d, &e);
    dif_make(g_dplain, g_dexp, DIF_APP2, DIF_REF2);
    DIF_CHECK("write CHECK_SWAP(app / ref を置き換え)", rc == BFM_OK &&
              bfm_read(c, DIF_OFF, g_draw, DIF_N * DIF_BS) == BFM_OK && memcmp(g_draw, g_dexp, DIF_N * DIF_BS) == 0);

    /* write CHECK_DELETE: R2 に 512 */
    d.op = BFM_DIF_CHECK_DELETE;
    rc = bfm_dif_write(c, DIF_R2, g_dpi, DIF_N, &d, &e);
    DIF_CHECK("write CHECK_DELETE(相手に 512)", rc == BFM_OK &&
              bfm_read(c, DIF_R2, g_draw, sizeof(g_dplain)) == BFM_OK && memcmp(g_draw, g_dplain, sizeof(g_dplain)) == 0);

    /* read INSERT: R2 の 512 -> 手元に 520 */
    d.op = BFM_DIF_INSERT;
    rc = bfm_dif_read(c, DIF_R2, g_dout, DIF_N, &d, &e);
    DIF_CHECK("read INSERT(手元で PI を作る)", rc == BFM_OK && memcmp(g_dout, g_dpi, DIF_N * DIF_BS) == 0);

    /* 誤りの検出。write: 手元のブロック 200 のデータを 1 バイト変える -> guard */
    d.op = BFM_DIF_CHECK;
    g_dpi[200u * DIF_BS + 17u] ^= 0x01u;
    memset(&e, 0, sizeof(e));
    rc = bfm_dif_write(c, DIF_OFF, g_dpi, DIF_N, &d, &e);
    snprintf(name, sizeof(name), "[8] %s: write CHECK でブロック 200 の guard の誤り", m);
    dif_expect_err(name, rc, &e, 200u, BFM_DIF_CHK_GUARD);
    g_dpi[200u * DIF_BS + 17u] ^= 0x01u;
    /* write: 期待する ref tag を 1 ずらす -> ブロック 0 の ref */
    d.ref_tag = DIF_REF + 1u;
    memset(&e, 0, sizeof(e));
    rc = bfm_dif_write(c, DIF_OFF, g_dpi, DIF_N, &d, &e);
    snprintf(name, sizeof(name), "[8] %s: write CHECK で ref tag の誤り(ブロック 0)", m);
    dif_expect_err(name, rc, &e, 0u, BFM_DIF_CHK_REF);
    d.ref_tag = DIF_REF;
    /* read: 相手の R1 に、ブロック 150 の app tag だけを変えたものを置く -> app */
    memcpy(g_draw, g_dpi, sizeof(g_dpi));
    g_draw[150u * DIF_BS + 515u] ^= 0x40u;
    rc = bfm_write(c, DIF_OFF, g_draw, DIF_N * DIF_BS);
    memset(&e, 0, sizeof(e));
    if (rc == BFM_OK) rc = bfm_dif_read(c, DIF_OFF, g_dout, DIF_N, &d, &e);
    snprintf(name, sizeof(name), "[8] %s: read CHECK でブロック 150 の app tag の誤り", m);
    dif_expect_err(name, rc, &e, 150u, BFM_DIF_CHK_APP);
    /* 同じものを app tag を検査せずに読めば通る(誤りの後も使えること)*/
    d.check = BFM_DIF_CHK_GUARD | BFM_DIF_CHK_REF;
    rc = bfm_dif_read(c, DIF_OFF, g_dout, DIF_N, &d, &e);
    DIF_CHECK("read CHECK(app tag を検査しない)で通る", rc == BFM_OK && memcmp(g_dout, g_draw, DIF_N * DIF_BS) == 0);
    d.check = BFM_DIF_CHK_ALL;
#undef DIF_CHECK
}

/* 起動と完了確認を分けた使い方(bfm_dif_*_start() / bfm_dif_poll() / bfm_dif_wait())。
 *   - write INSERT を起動したまま、2 本目の起動が BFM_E_BUSY になること、途中で同じ接続の
 *     普通の write / read が使えること、bfm_dif_poll() で最後まで進むこと
 *   - read CHECK を起動し、bfm_dif_wait() で 3 つ目の分(2 面目を使い回す分)の誤りを受け取ること */
#define DIF_AUX (DIF_R2 + DIF_N * BFM_DIF_BLOCK)   /* 普通の write / read に使う所(R2 の後ろ)*/

static void dif_async_tests(bfm_conn_t *c, uint8_t mode)
{
    const char *m = (mode == BFM_DIF_HW) ? "HW" : "SW";
    char name[128];
    bfm_dif_t d;
    bfm_dif_err_t e;
    memset(&d, 0, sizeof(d));
    memset(&e, 0, sizeof(e));
    d.mode = mode;
    d.check = BFM_DIF_CHK_ALL;
    d.app_tag = DIF_APP;
    d.ref_tag = DIF_REF;
    d.op = BFM_DIF_INSERT;
    dif_make(g_dplain, g_dexp, DIF_APP, DIF_REF);

    const int rc = bfm_dif_write_start(c, DIF_OFF, g_dplain, DIF_N, &d);
    const int busy = bfm_dif_read_start(c, DIF_OFF, g_dout, DIF_N, &d);
    uint8_t pat[64], back[64];
    for (unsigned i = 0; i < sizeof(pat); i++) pat[i] = (uint8_t)(0xa5u ^ i ^ mode);
    memset(back, 0, sizeof(back));
    const int rw = bfm_write(c, DIF_AUX, pat, sizeof(pat));
    const int rr = bfm_read(c, DIF_AUX, back, sizeof(back));
    uint32_t inprog = 0;
    int r;
    while ((r = bfm_dif_poll(c, &e)) == BFM_DIF_INPROGRESS && !g_stop) inprog++;
    const int idle = bfm_dif_poll(c, &e);
    snprintf(name, sizeof(name), "[8] %s: 分けた API: write INSERT を起動・poll で完了(途中 %u 回)", m, inprog);
    check(name, rc == BFM_OK && r == BFM_OK && inprog > 0 &&
                bfm_read(c, DIF_OFF, g_draw, DIF_N * DIF_BS) == BFM_OK && memcmp(g_draw, g_dexp, DIF_N * DIF_BS) == 0);
    snprintf(name, sizeof(name), "[8] %s: 分けた API: 途中の 2 本目の起動は BFM_E_BUSY(rc=%d)", m, busy);
    check(name, busy == BFM_E_BUSY);
    snprintf(name, sizeof(name), "[8] %s: 分けた API: 途中で普通の write / read が使える", m);
    check(name, rw == BFM_OK && rr == BFM_OK && memcmp(pat, back, sizeof(pat)) == 0);
    snprintf(name, sizeof(name), "[8] %s: 分けた API: 動いていなければ bfm_dif_poll() は BFM_OK", m);
    check(name, idle == BFM_OK);

    /* 相手の R1 のブロック 280(3 つ目の分 = 面 0 の 2 回目)の app tag を壊し、read CHECK で見つける */
    const uint32_t bad = 280u;
    uint8_t tag[2] = {0xde, 0xad};
    d.op = BFM_DIF_CHECK;
    memset(g_dout, 0, sizeof(g_dout));
    int r2 = bfm_write(c, DIF_OFF + (uint64_t)bad * DIF_BS + 514u, tag, 2);
    if (r2 == BFM_OK) r2 = bfm_dif_read_start(c, DIF_OFF, g_dout, DIF_N, &d);
    if (r2 == BFM_OK) r2 = bfm_dif_wait(c, &e);
    snprintf(name, sizeof(name), "[8] %s: 分けた API: 3 つ目の分の app tag の誤りを bfm_dif_wait() で受け取る", m);
    dif_expect_err(name, r2, &e, bad, BFM_DIF_CHK_APP);
    snprintf(name, sizeof(name), "[8] %s: 分けた API: 誤りの前のブロック(0〜%u)は dst に届いている", m, bad - 1u);
    check(name, memcmp(g_dout, g_dexp, (size_t)bad * DIF_BS) == 0);
}

/* DIF のスループット(DIF_N ブロックずつ、1 回ずつ完了を待つ。データ 512 バイト分で数える)。 */
static void dif_tput(bfm_conn_t *c, uint8_t mode, uint8_t op, int is_read, uint32_t reps)
{
    bfm_dif_t d;
    memset(&d, 0, sizeof(d));
    d.op = op;
    d.mode = mode;
    d.check = BFM_DIF_CHK_ALL;
    d.app_tag = DIF_APP;
    d.ref_tag = DIF_REF;
    const void *src = (op == BFM_DIF_INSERT) ? (const void *)g_dplain : (const void *)g_dpi;
    const uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < reps; i++) {
        const int rc = is_read ? bfm_dif_read(c, DIF_OFF, g_dout, DIF_N, &d, NULL)
                               : bfm_dif_write(c, DIF_OFF, src, DIF_N, &d, NULL);
        if (rc != BFM_OK) {
            printf("client: DIF のスループット: %s(rc=%d)\n", bfm_strerror(rc), rc);
            g_ng++;
            return;
        }
    }
    const double ns = (double)(now_ns() - t0);
    static const char *ops[] = {"CHECK", "CHECK_SWAP", "INSERT", "CHECK_DELETE"};
    printf("client: スループット DIF %s %-5s %-12s %u ブロック x%u: %.1f MB/s(1 回 %.1f us)\n",
           mode == BFM_DIF_HW ? "HW" : "SW", is_read ? "read" : "write", ops[op], DIF_N, reps,
           (double)DIF_N * BFM_DIF_BLOCK * reps / ns * 1000.0, ns / reps / 1000.0);
}

/* DIF の遅延(8 ブロック = 4KiB、1 回ずつ完了を待つ)を SW / HW で比べる。 */
static void dif_lat(bfm_conn_t *c, uint8_t mode, uint8_t op, int is_read, uint32_t n)
{
    bfm_dif_t d;
    memset(&d, 0, sizeof(d));
    d.op = op;
    d.mode = mode;
    d.check = BFM_DIF_CHK_ALL;
    d.app_tag = DIF_APP;
    d.ref_tag = DIF_REF;
    const uint64_t remote = DIF_OFF;                     /* 520 バイトおきの形(read INSERT は測らない)*/
    if (n > sizeof(g_lat) / sizeof(g_lat[0])) n = sizeof(g_lat) / sizeof(g_lat[0]);
    dif_make(g_dplain, g_dpi, DIF_APP, DIF_REF);
    for (uint32_t i = 0; i < n + 100u; i++) {
        const uint64_t t0 = now_ns();
        const int rc = is_read ? bfm_dif_read(c, remote, g_dout, 8u, &d, NULL)
                               : bfm_dif_write(c, remote, op == BFM_DIF_INSERT ? (const void *)g_dplain
                                                                              : (const void *)g_dpi, 8u, &d, NULL);
        if (rc != BFM_OK) {
            printf("client: DIF の遅延: %s(rc=%d)\n", bfm_strerror(rc), rc);
            g_ng++;
            return;
        }
        if (i >= 100u) g_lat[i - 100u] = (uint32_t)(now_ns() - t0);
    }
    qsort(g_lat, n, sizeof(g_lat[0]), cmp_u32);
    static const char *ops[] = {"CHECK", "CHECK_SWAP", "INSERT", "CHECK_DELETE"};
    printf("client: 遅延 DIF %s %-5s %-12s 4KiB x%u: 中央値 %u ns / p99 %u\n", mode == BFM_DIF_HW ? "HW" : "SW",
           is_read ? "read" : "write", ops[op], n, g_lat[n / 2u], g_lat[(uint64_t)n * 99u / 100u]);
}

static void run_dif(bfm_conn_t *c, uint32_t iters)
{
    if (bfm_remote_size(c) < DIF_R2 + DIF_N * DIF_BS) {
        printf("client: [8] 相手の公開メモリが小さいので T10-DIF の検査を飛ばす\n");
        return;
    }
    printf("client: [8] T10-DIF(%u ブロック)。オフロード %s\n", DIF_N,
           bfm_dif_hw_supported() ? "あり(SW と HW を試す)" : "なし(SW だけ)");
    dif_tests(c, BFM_DIF_SW);
    dif_async_tests(c, BFM_DIF_SW);
    if (bfm_dif_hw_supported()) {
        dif_tests(c, BFM_DIF_HW);
        dif_async_tests(c, BFM_DIF_HW);
    } else {
        bfm_dif_t d;
        memset(&d, 0, sizeof(d));
        d.mode = BFM_DIF_HW;
        check("[8] オフロードなしの装置で BFM_DIF_HW は BFM_E_NOTSUP",
              bfm_dif_write(c, DIF_OFF, g_dplain, 1, &d, NULL) == BFM_E_NOTSUP);
    }
    if (g_stop || g_ng) return;
    const uint32_t n = iters < 5000u ? iters : 5000u;
    for (uint8_t mode = BFM_DIF_SW; mode <= BFM_DIF_HW; mode++) {
        if (mode == BFM_DIF_HW && !bfm_dif_hw_supported()) break;
        dif_lat(c, mode, BFM_DIF_INSERT, 0, n);
        dif_lat(c, mode, BFM_DIF_CHECK, 0, n);
        dif_lat(c, mode, BFM_DIF_CHECK, 1, n);
        dif_lat(c, mode, BFM_DIF_CHECK_DELETE, 1, n);
    }
    /* スループット: 相手の R1 を作り直してから(write INSERT の後は R1 = g_dpi)*/
    const uint32_t reps = n / 2u ? n / 2u : 1u;
    dif_make(g_dplain, g_dpi, DIF_APP, DIF_REF);
    for (uint8_t mode = BFM_DIF_SW; mode <= BFM_DIF_HW; mode++) {
        if (mode == BFM_DIF_HW && !bfm_dif_hw_supported()) break;
        dif_tput(c, mode, BFM_DIF_INSERT, 0, reps);
        dif_tput(c, mode, BFM_DIF_CHECK, 0, reps);
        dif_tput(c, mode, BFM_DIF_CHECK, 1, reps);
        dif_tput(c, mode, BFM_DIF_CHECK_DELETE, 1, reps);
    }
}

/* ---------------------------------------------------------------------
 * mt: 複数のコア(スレッド)から同時に転送する
 *
 * スレッドごとに別の接続を bfm_connect() で同時に作り、相手の公開メモリの
 * MT_OFF + スレッドの番号 x MT_SPAN(スレッドごとに別の区画)で次を確かめる。
 *   [1] 4KiB の write(中継バッファから)と read で読み戻す(64 回)
 *   [2] 8 バイトの write と read で読み戻す
 *   [3] 全スレッドが同じ語へ FAA を n 回ずつ打つ -> 最後に n x スレッド数になる(接続をまたいだ atomic)
 *   [4] T10-DIF(SW)の write INSERT と read CHECK_DELETE で元のデータに戻る(起動と完了確認を分けた形)
 * そのあと全スレッドで同時に、8 バイトの write / read / FAA(自分の語)を n 回ずつ打ち、合計の速さを出す。
 * 間ずっと別のスレッドが bfm_poll() を回す(鍵を取る側も同時に動かす)。最後に全スレッドが同時に切断する。
 * スレッドは CPU に 1 つずつ固定する(CPU の数より多ければ重ねる)。
 * ------------------------------------------------------------------- */

#define MT_OFF     (4u * 1024u * 1024u)
#define MT_SPAN    (64u * 1024u)
#define MT_CNT_OFF (MT_OFF + BFM_MAX_CONN * MT_SPAN)   /* 全スレッドで共有する FAA の語 */
#define MT_DIF_N   16u

struct mt_arg {
    pthread_t th;                        /* このスレッド */
    int idx;                             /* スレッドの番号(区画の位置と CPU を決める)*/
    uint32_t ip;                         /* 相手の IPv4 */
    const uint8_t *mac;                  /* 相手の MAC */
    uint16_t port;                       /* 接続の番号 */
    bfm_conn_t *c;                       /* このスレッドの接続(NULL = 接続できなかった)*/
    int ng;                              /* NG の数 */
    char why[200];                       /* 最初の NG の内容 */
    uint64_t ns[3];                      /* 速さの区間(write / read / FAA)にかかった時間 */
};

static pthread_barrier_t g_mt_bar;       /* 全スレッドで足並みをそろえる(同時に接続・測る・切断する)*/
static char g_mt_msg[200];               /* 結果の行を組み立てる置き場 */
static volatile int g_mt_poll_stop;      /* 1 = bfm_poll() を回すスレッドを止める */
static uint32_t g_mt_n;                  /* 1 本のスレッドが速さの区間で打つ回数(-n)*/
static uint32_t g_mt_threads;            /* スレッドの数(-t)*/
static uint64_t g_mt_counter;            /* [3] の結果(スレッド 0 が読む)*/

static void mt_fail(struct mt_arg *a, const char *what, int rc)
{
    if (a->ng++ == 0) snprintf(a->why, sizeof(a->why), "%s(rc=%d %s)", what, rc, bfm_strerror(rc));
}

static void *mt_poller(void *p)
{
    (void)p;
    while (!g_mt_poll_stop) {
        (void)bfm_poll();
        usleep(200);
    }
    return NULL;
}

static void mt_check(struct mt_arg *a)
{
    bfm_conn_t *c = a->c;
    const uint64_t base = MT_OFF + (uint64_t)a->idx * MT_SPAN;
    static __thread uint8_t wbuf[4096], rbuf[4096];
    int rc;

    /* [1] 4KiB の write / read */
    for (uint32_t k = 0; k < 64u && !a->ng; k++) {
        for (uint32_t i = 0; i < sizeof(wbuf); i++) wbuf[i] = (uint8_t)(a->idx * 31u + k * 7u + i);
        if ((rc = bfm_write(c, base, wbuf, sizeof(wbuf))) != BFM_OK) { mt_fail(a, "[1] write 4KiB", rc); break; }
        memset(rbuf, 0, sizeof(rbuf));
        if ((rc = bfm_read(c, base, rbuf, sizeof(rbuf))) != BFM_OK) { mt_fail(a, "[1] read 4KiB", rc); break; }
        if (memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) mt_fail(a, "[1] 4KiB を読み戻すと違う", 0);
    }
    /* [2] 8 バイトの write / read */
    for (uint32_t k = 0; k < 1000u && !a->ng; k++) {
        const uint64_t v = ((uint64_t)a->idx << 32) | k, off = base + 4096u + (k & 63u) * 8u;
        uint64_t r = 0;
        if ((rc = bfm_write(c, off, &v, 8)) != BFM_OK) { mt_fail(a, "[2] write 8B", rc); break; }
        if ((rc = bfm_read(c, off, &r, 8)) != BFM_OK) { mt_fail(a, "[2] read 8B", rc); break; }
        if (r != v) mt_fail(a, "[2] 8 バイトを読み戻すと違う", 0);
    }
}

static void mt_dif(struct mt_arg *a)
{
    bfm_conn_t *c = a->c;
    const uint64_t base = MT_OFF + (uint64_t)a->idx * MT_SPAN + 8192u;
    static __thread uint8_t plain[MT_DIF_N * BFM_DIF_BLOCK], out[MT_DIF_N * BFM_DIF_BLOCK];
    bfm_dif_t d;
    bfm_dif_err_t e;
    memset(&d, 0, sizeof(d));
    d.mode = BFM_DIF_SW;
    d.check = BFM_DIF_CHK_ALL;
    d.app_tag = (uint16_t)(0x1000u + a->idx);
    d.ref_tag = 0x500000u + (uint32_t)a->idx * 1000u;
    for (uint32_t i = 0; i < sizeof(plain); i++) plain[i] = (uint8_t)(a->idx ^ (i * 13u));
    d.op = BFM_DIF_INSERT;
    int rc = bfm_dif_write_start(c, base, plain, MT_DIF_N, &d);
    if (rc == BFM_OK) {
        do {
            rc = bfm_dif_poll(c, &e);
        } while (rc == BFM_DIF_INPROGRESS);
    }
    if (rc != BFM_OK) { mt_fail(a, "[4] DIF write INSERT", rc); return; }
    d.op = BFM_DIF_CHECK_DELETE;
    memset(out, 0, sizeof(out));
    rc = bfm_dif_read_start(c, base, out, MT_DIF_N, &d);
    if (rc == BFM_OK) rc = bfm_dif_wait(c, &e);
    if (rc != BFM_OK) { mt_fail(a, "[4] DIF read CHECK_DELETE", rc); return; }
    if (memcmp(plain, out, sizeof(plain)) != 0) mt_fail(a, "[4] DIF で元のデータに戻らない", 0);
}

static void *mt_worker(void *p)
{
    struct mt_arg *a = (struct mt_arg *)p;
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET((int)(a->idx % (ncpu > 0 ? ncpu : 1)), &cs);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    pthread_barrier_wait(&g_mt_bar);                         /* 全スレッドで同時に接続する */
    a->c = do_connect(a->ip, a->mac, a->port, 20000u);
    if (a->c == NULL) mt_fail(a, "接続できない", 0);
    pthread_barrier_wait(&g_mt_bar);
    int rc;
    if (a->idx == 0 && a->c) {                               /* 共有の語を 0 に */
        const uint64_t z = 0;
        if ((rc = bfm_write(a->c, MT_CNT_OFF, &z, 8)) != BFM_OK) mt_fail(a, "[3] 共有の語を 0 にする", rc);
    }
    pthread_barrier_wait(&g_mt_bar);
    if (a->c && !a->ng) mt_check(a);
    for (uint32_t k = 0; k < g_mt_n && a->c && !a->ng; k++) {   /* [3] 共有の語へ FAA */
        uint64_t old;
        if ((rc = bfm_faa(a->c, MT_CNT_OFF, 1, &old)) != BFM_OK) mt_fail(a, "[3] FAA", rc);
    }
    if (a->c && !a->ng) mt_dif(a);
    pthread_barrier_wait(&g_mt_bar);
    if (a->idx == 0 && a->c) {
        if ((rc = bfm_read(a->c, MT_CNT_OFF, &g_mt_counter, 8)) != BFM_OK) mt_fail(a, "[3] 共有の語を読む", rc);
    }

    /* 速さ: write / read / FAA(自分の語)を全スレッドで同時に */
    const uint64_t own = MT_OFF + (uint64_t)a->idx * MT_SPAN + 4096u + 512u;
    for (int ph = 0; ph < 3; ph++) {
        pthread_barrier_wait(&g_mt_bar);
        const uint64_t t0 = now_ns();
        for (uint32_t k = 0; k < g_mt_n && a->c && !a->ng; k++) {
            uint64_t v = k, old;
            rc = (ph == 0) ? bfm_write(a->c, own, &v, 8) : (ph == 1) ? bfm_read(a->c, own, &v, 8)
                                                                      : bfm_faa(a->c, own + 8u, 1, &old);
            if (rc != BFM_OK) mt_fail(a, ph == 0 ? "速さ write" : ph == 1 ? "速さ read" : "速さ FAA", rc);
        }
        a->ns[ph] = now_ns() - t0;
    }
    pthread_barrier_wait(&g_mt_bar);                         /* 全スレッドで同時に切断する */
    if (a->c && do_disconnect(a->c) != BFM_OK) mt_fail(a, "切断", 0);
    return NULL;
}

static int run_mt(uint32_t ip, const uint8_t mac[6], uint16_t port, uint32_t nthreads, uint32_t n)
{
    static struct mt_arg args[BFM_MAX_CONN];
    if (nthreads == 0 || nthreads > g_bfm.nconn) {
        printf("mt: スレッドの数は 1〜%u(-t)\n", g_bfm.nconn);
        return 2;
    }
    g_mt_n = n;
    g_mt_threads = nthreads;
    g_mt_poll_stop = 0;
    printf("mt: スレッド %u 本(CPU %ld 個)、各 %u 回\n", nthreads, sysconf(_SC_NPROCESSORS_ONLN), n);
    pthread_barrier_init(&g_mt_bar, NULL, nthreads);
    pthread_t poller;
    pthread_create(&poller, NULL, mt_poller, NULL);
    for (uint32_t t = 0; t < nthreads; t++) {
        memset(&args[t], 0, sizeof(args[t]));
        args[t].idx = (int)t;
        args[t].ip = ip;
        args[t].mac = mac;
        args[t].port = port;
        pthread_create(&args[t].th, NULL, mt_worker, &args[t]);
    }
    for (uint32_t t = 0; t < nthreads; t++) pthread_join(args[t].th, NULL);
    g_mt_poll_stop = 1;
    pthread_join(poller, NULL);
    pthread_barrier_destroy(&g_mt_bar);

    uint32_t ng = 0, up = 0;
    for (uint32_t t = 0; t < nthreads; t++) {
        if (args[t].c) up++;
        if (args[t].ng) {
            ng++;
            printf("mt: スレッド %u: NG %s\n", t, args[t].why);
        }
    }
    snprintf(g_mt_msg, sizeof(g_mt_msg), "%u 本のスレッドが同時に接続した(%u 本)", nthreads, up);
    check(g_mt_msg, up == nthreads);
    snprintf(g_mt_msg, sizeof(g_mt_msg), "[1][2][4] スレッドごとの write / read / T10-DIF(NG のスレッド %u 本)", ng);
    check(g_mt_msg, ng == 0);
    snprintf(g_mt_msg, sizeof(g_mt_msg), "[3] 共有の語への FAA の合計 = %llu(あるべき %llu)",
             (unsigned long long)g_mt_counter, (unsigned long long)n * nthreads);
    check(g_mt_msg, g_mt_counter == (uint64_t)n * nthreads);
    if (ng == 0) {
        static const char *names[] = {"write 8B", "read 8B", "FAA"};
        for (int ph = 0; ph < 3; ph++) {
            uint64_t worst = 0, sum = 0;
            for (uint32_t t = 0; t < nthreads; t++) {
                if (args[t].ns[ph] > worst) worst = args[t].ns[ph];
                sum += args[t].ns[ph];
            }
            printf("mt: 速さ %-8s スレッド %2u 本: 合計 %.2f Mops(1 本あたり %.2f Mops、1 回 %.0f ns)\n", names[ph],
                   nthreads, (double)n * nthreads / (double)worst * 1000.0,
                   (double)n / ((double)sum / nthreads) * 1000.0, (double)sum / nthreads / n);
        }
    }
    printf("mt: 結果 %s(NG %d 件)\n", g_ng ? "NG" : "OK", g_ng);
    return g_ng ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * bench: 多重度(同時に出しておく数)ごとのスループット・IOPS・遅延
 * ------------------------------------------------------------------- */

#define BENCH_QD_MAX   16u
#define BENCH_SIZE_MAX (256u * 1024u)

static double g_tsc_per_ns;              /* 1ns あたりの TSC の数(bench の時間を TSC で測る)*/

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
    bfm_conn_t *c = do_connect(ip, mac, port, 0);
    if (c == NULL) return 1;
    tsc_calibrate();
    for (unsigned i = 0; i < sizeof(g_src); i++) g_src[i] = (uint8_t)(0x5a ^ i);
    {
        /* 埋め込みでない write は中継バッファから送るので、送る中身を置いておく */
        struct bfm_conn *ci = (struct bfm_conn *)c;
        for (uint32_t i = 0; i < ci->stage_size; i++) ci->stage[i] = (uint8_t)(0x5a ^ i);
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
    (void)do_disconnect(c);
    return ng;
}

int main(int argc, char **argv)
{
    uint16_t port = 0;
    uint32_t iters = 0, threads = 4;
    int oob = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d:i:p:m:M:n:t:BSCxvh")) != -1) {
        switch (opt) {
        case 'd': g_bfm_linux_pci_bdf = optarg; break;
        case 'i': if (parse_ip(optarg, &g_bfm_local_ip) != 0) { usage(); return 2; } break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 'm': g_bfm_path_mtu = (uint8_t)atoi(optarg); break;
        case 'M': g_bfm_port_mtu = (uint16_t)atoi(optarg); break;
        case 'n': iters = (uint32_t)atoi(optarg); break;
        case 't': threads = (uint32_t)atoi(optarg); break;
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

    const int bench = (strcmp(mode, "bench") == 0), mt = (strcmp(mode, "mt") == 0);
    g_server = (strcmp(mode, "server") == 0);
    if (bench) {
        g_bfm_stage_size = BENCH_QD_MAX * BENCH_SIZE_MAX;   /* 多重度ぶんの着地点 / 送り元 */
        g_bfm_max_conn = 1;                                 /* 中継バッファが大きいので枠は 1 つだけ */
    }
    if (do_init() != BFM_OK) return 1;
    int ret = 0;
    if (strcmp(mode, "server") == 0) {
        ret = run_server(port);
    } else if ((strcmp(mode, "client") == 0 || bench || mt) && optind + 1 < argc) {
        uint32_t ip;
        uint8_t mac[6] = {0, 0, 0, 0, 0, 0};       /* 全部 0 = 自動(bfm_connect() が REQ を同報で送る)*/
        if (parse_ip(argv[optind + 1], &ip) != 0 ||
            (optind + 2 < argc && parse_mac(argv[optind + 2], mac) != 0)) {
            usage();
            ret = 2;
        } else if (bench) {
            ret = run_bench(ip, mac, port, iters ? iters : 100000u);
        } else if (mt) {
            ret = run_mt(ip, mac, port, threads, iters ? iters : 100000u);
        } else {
            ret = run_client(ip, mac, port, iters ? iters : 20000u, oob);
        }
    } else if (strcmp(mode, "info") == 0) {
        printf("info: %s bf=%u bf_buf=%u atomic ops=0x%x size_qp=0x%x mode=%u ts_format=%u "
               "read/atomic req/res=%u/%u FW ページ %u sho=%u T10-DIF オフロード %s\n", g_bfm.name,
               g_bfm.bf_cap, g_bfm.bf_buf_size, g_bfm.atomic_ops, g_bfm.atomic_size_qp, g_bfm.atomic_mode,
               g_bfm.qp_ts_format, 1u << g_bfm.log_max_ra_req_qp, 1u << g_bfm.log_max_ra_res_qp,
               g_bfm.fw_pages_used, g_bfm.sho, bfm_dif_hw_supported() ? "あり" : "なし");
    } else {
        usage();
        ret = 2;
    }
    do_shutdown();
    return ret;
}
