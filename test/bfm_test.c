/* bfmem の試験プログラム(Linux + VFIO)。**両方のノードで同じものを動かす**(双方向)。
 *
 *   bfmem_test [オプション] run <相手のIP> [ポート1の相手のIP]
 *       代表 API bfm_run() を -t 本のスレッドから同時に呼び続けて、全部の接続を張る。張れたら
 *       両方のノードが同時に、相手の公開メモリへ一通りの操作をして確かめ(接続 (0,0))、続けて
 *       接続ごとに 1 本のスレッドで同時に転送する(全部の接続。mt)。最後に相手と足並みをそろえて止める。
 *   bfmem_test [オプション] serve <相手のIP> [ポート1の相手のIP]
 *       接続を張って保つだけ(bench の相手)。Ctrl-\(SIGQUIT)で自分の公開メモリの hash を出す
 *   bfmem_test [オプション] bench <相手のIP>    接続 1 本で多重度ごとの速さを測る(相手は serve -c 1)
 *   bfmem_test [オプション] info <相手のIP>     ポートを立ち上げて能力を表示するだけ
 *
 * ポート 1 を使うのは、ポート 1 の相手の IP を書いたとき(-D・-I でポート 1 の装置と自分の IP)。
 * 掛け側・受け側は IP の大小で自動で決まる(小さいほうが掛け側)。相手の MAC は自動で取る。
 *
 * 公開メモリの取り決め(どちらのノードも、自分の公開メモリを最初に埋める):
 *   [0, 64KiB)          データ領域。(i * 7 + 3) & 0xff で埋める
 *   [64KiB, 64KiB+4KiB) 8 バイトの atomic 語 x 512(0 で埋める)
 *   SYNC_OFF            足並みをそろえる語(相手が自分の進み具合を書きに来る。0 で埋める)
 * 相手のデータ領域の中身を自分でも持っておき(模型)、最後に読み戻して突き合わせ、
 * FNV-1a 64 の hash を表示する。止める前に自分の公開メモリの hash も表示するので、
 * 「相手の模型の hash」と「自分の公開メモリの hash」が同じことを、両方の向きで見る
 * (両側が同じ間違い方をしても気付けるように)。 */
#define _GNU_SOURCE
#include "bfm.h"
#include "../src/bfm_internal.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DATA_BYTES   65536u
#define ATOM_OFF     65536u
#define ATOM_WORDS   512u
#define AREA_BYTES   (ATOM_OFF + ATOM_WORDS * 8u)
#define SYNC_OFF     (3u * 1024u * 1024u)   /* 足並みをそろえる語(相手が書きに来る)*/

static volatile sig_atomic_t g_stop; /* 止める合図(SIGINT / SIGTERM、serve 以外では SIGQUIT も)*/
static volatile sig_atomic_t g_dump; /* hash を出す合図(serve で SIGQUIT)*/
static int g_serve;                  /* serve のときだけ SIGQUIT で hash を出す */
static int g_quiet;                  /* -q: こちらからは転送せず、相手からの転送を受けるだけ(片方向の速さを測るため)*/
static int g_perf;                   /* -P: run で一通りの検査を飛ばし、mt(全部の接続での転送と速さ)だけを行う */
static int g_steplog;                /* -L: 最後(止めた後)に全部の接続の状態遷移の記録を表示する */
static unsigned g_wp_rot;            /* -R: wpost でコア k に割り当てる接続を、k + R 番目のものにずらす(偏りの元を切り分ける)*/
static int g_silent;                 /* -z: [10] 無応答の試験(両方のノードに付ける。IP の小さいほうが試し、大きいほうが黙る)*/
static uint8_t g_model[DATA_BYTES];  /* 相手のデータ領域にあるはずの中身(模型)*/

static void on_sig(int s)
{
    if (s == SIGQUIT && g_serve) g_dump = 1;
    else g_stop = 1;                 /* serve 以外では SIGQUIT も止める合図(後片付けをしてから終わる)*/
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

static void usage(void)
{
    printf("使い方: bfmem_test [-d BDF] [-i 自分のIP] [-D ポート1のBDF] [-I ポート1の自分のIP] [-c 接続数/ポート]\n"
           "                   [-t スレッド数] [-p 接続の番号] [-m 経路MTU 1-5] [-M ポートのMTU] [-n 回数]\n"
           "                   [-Q 多重度 1-64] [-a ack timeout] [-R ずらし] [-T 項目名[:長さ]] [-B] [-S] [-C] [-x] [-q] [-P] [-z] [-L] [-v]\n"
           "                   run <相手のIP> [ポート1の相手のIP] | serve <相手のIP> [ポート1の相手のIP] |\n"
           "                   race <相手のIP> [ポート1の相手のIP] | wpost <相手のIP> [ポート1の相手のIP] |\n"
           "                   perf <相手のIP> [ポート1の相手のIP] | crc |\n"
           "                   bench <相手のIP> | info <相手のIP> | [-A] fw <相手のIP> <FW の像の .bin>\n"
           "  両方のノードで同じものを動かす。IP の小さいほうが掛け側になる(自動)。相手の MAC も自動で取る\n"
           "  run: -t 本(既定 4)のスレッドが bfm_run() を同時に呼んで全部の接続(-c 本 x ポート。既定 16)を張り、\n"
           "       両方向に一通りの操作を確かめ、全部の接続で同時に write / read / atomic / T10-DIF を確かめ、\n"
           "       8 バイトの write / read / FAA を -n 回(既定 100000)ずつ、64KiB の write / read を -n/50 回ずつ\n"
           "       同時に打って合計の速さを出す(区間ごとに相手と足並みをそろえるので、相手も同時に打つ = 両方向)。\n"
           "       続けて多重度つき(bfm_*_async。-Q 個まで出しておく)の 8 バイトの write(ACK なし / あり)・read を\n"
           "       -n 回ずつ、4KiB の write / read を -n/10 回ずつ、FAA・CAS を -n 回ずつ打つ\n"
           "  race: 競合の試験(両方のノードで動かす)。-t 本が bfm_run() を休まず呼ぶ間、各ポートの前半の接続で転送を\n"
           "        確かめ続け、後半の接続で張り直しを繰り返す(-n 秒。既定 30)。続けて bfm_run() を回したまま\n"
           "        bfm_shutdown() し、回しているスレッドに立ち上げ直させる\n"
           "  wpost: 全部のコアに 1 本ずつスレッドを固定し、コアごとに接続 1 本で 8 バイトの bfm_write_post() を\n"
           "         -n 秒(既定 5)x 3 回、同時に打つ(両方のノードで動かす = 両方向)。コアごとの速さと偏りを出す\n"
           "  perf: コアの数 1〜4 で、write_post・read・read_async・CAS・FAA・DIF の write / read(CHECK)の\n"
           "        IOPS・MB/s・平均の遅延を、長さごとに -n 秒(既定 2)ずつ測る(両方のノードで動かす = 両方向)\n"
           "  crc: T10-DIF の CRC16 だけを確かめる(相手も NIC も要らない)。PCLMULQDQ・表・1 ビットずつの値の一致と、\n"
           "       512 バイトあたりの速さ(表 / 使う方法)を -n 秒(既定 1)ずつ\n"
           "  fw: ポート 0 の装置のフラッシュへ FW の像(.bin)を FW 自身に書かせ(bfm_fw_update())、確かめさせる。\n"
           "      既定は確かめるだけ(切り替えずに取り消す。次の起動の FW は変わらない)。-A で次の起動から使うよう\n"
           "      切り替える(電源を入れ直すと有効になる)。Ctrl-C で取り消す。相手は動いていなくてよい\n"
           "  bench: write / read を 4B〜256KiB、多重度 1・4・8・16 で -n 回(既定 100000)ずつ流し、\n"
           "         IOPS・MB/s・平均遅延を出す(接続 1 本。相手は serve -c 1)\n"
           "  -B  BlueFlame を使わない(8 バイトのドアベルだけ。比べるため)\n"
           "  -S  read / atomic の応答を CQE に書かせない(scatter to CQE を使わない)\n"
           "  -C  CPU を深い省電力状態に入れない設定(g_bfm_cpu_awake)を切る\n"
           "  -x  最後に公開メモリの範囲の外を読み、相手の NIC が断ることを確かめ、その接続を張り直す\n"
           "  -q  run で、こちらからは転送せず相手からの転送を受けるだけ(片方向の速さを測るとき、受ける側に付ける)\n"
           "  -P  run で一通りの検査を飛ばし、全部の接続での転送と速さ(mt)だけを行う(本数を変えて速さを測るとき)\n"
           "  -z  run で無応答の試験([10]。両方のノードに付ける)。IP の大きいほうが最後の 4 本の QP を BYE なしで消し、\n"
           "      小さいほうが ACK・応答の返らない状態で、資源の空き待ち・bfm_check()・bfm_wait() の時間切れと、\n"
           "      NIC の再送の間隔(bfm_write() が BFM_E_RETRY になるまでの時間)を確かめて張り直す\n"
           "  -a N  RC の ack timeout(g_bfm_ack_timeout。4.096us x 2^N。既定 12)\n"
           "  -L  最後(止めた後)に、全部の接続の状態遷移の記録(移った時刻・とどまった時間)を表示する\n"
           "  -R N  wpost で、コア k に (k + N) 番目の接続を割り当てる(既定 0。遅さがコアと接続のどちらに付くかを見る)\n"
           "  -T 項目名[:長さ]  perf で、名前にこれを含む項目だけを測る(:長さ を付ければ、その長さだけ)。-q を付けた perf は\n"
           "        打たずに足並みだけそろえる(相手だけが打つ片方向)\n");
}

/* ---------------------------------------------------------------------
 * 代表 API を回す
 * ------------------------------------------------------------------- */

/* 呼び直すまでの合間(本番ではここでほかの仕事をする)。 */
static void idle(void)
{
    usleep(50);
}

static void do_shutdown(void)
{
    while (bfm_shutdown() == BFM_INPROGRESS) idle();
}

/* bfm_run() を回すスレッド(-t 本)。全部の接続が使えるか、失敗か、止める合図まで呼び続ける。 */
struct up_arg {
    pthread_t th;                        /* このスレッド */
    int idx;                             /* スレッドの番号(CPU を決める)*/
    uint64_t calls;                      /* bfm_run() を呼んだ回数 */
    int rc;                              /* 最後の戻り値 */
};

static volatile int g_up_done;           /* 1 = 誰かが BFM_OK か失敗を見た(全員やめる)*/

static void reconnect_down(void);

static void *up_worker(void *p)
{
    struct up_arg *a = (struct up_arg *)p;
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET((int)(a->idx % (ncpu > 0 ? ncpu : 1)), &cs);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    while (!g_up_done && !g_stop) {
        a->rc = bfm_run();
        a->calls++;
        if (a->rc == BFM_OK || (a->rc < 0 && a->rc != BFM_E_STATE)) g_up_done = 1;
        else {
            if (a->rc == BFM_E_STATE) reconnect_down();   /* 立ち上げの途中で切れた接続は張り直す */
            idle();
        }
    }
    return NULL;
}

/* 全部の接続が使えるようになるまで、nthreads 本のスレッドから bfm_run() を同時に呼ぶ。
 * 戻り値: BFM_OK か失敗の理由。 */
static int run_up(uint32_t nthreads)
{
    static struct up_arg args[64];
    if (nthreads == 0) nthreads = 1;
    if (nthreads > 64u) nthreads = 64u;
    const uint64_t t0 = now_ns();
    g_up_done = 0;
    for (uint32_t t = 0; t < nthreads; t++) {
        memset(&args[t], 0, sizeof(args[t]));
        args[t].idx = (int)t;
        args[t].rc = BFM_INPROGRESS;
        pthread_create(&args[t].th, NULL, up_worker, &args[t]);
    }
    int rc = BFM_INPROGRESS;
    for (uint32_t t = 0; t < nthreads; t++) {
        pthread_join(args[t].th, NULL);
        if (args[t].rc == BFM_OK || (args[t].rc < 0 && args[t].rc != BFM_E_STATE)) rc = args[t].rc;
    }
    if (rc == BFM_INPROGRESS) rc = bfm_run();
    printf("run: bfm_run() を %u 本のスレッドから呼んだ(", nthreads);
    for (uint32_t t = 0; t < nthreads; t++)
        printf("%s%llu", t ? " / " : "", (unsigned long long)args[t].calls);
    printf(" 回)。%.2f 秒で %s\n", (double)(now_ns() - t0) / 1e9, rc == BFM_OK ? "全部の接続が使える" : bfm_strerror(rc));
    return rc;
}

/* 使う接続の数(全部のポート)。 */
static unsigned conn_total(void)
{
    unsigned n = 0;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) n += g_bfm_port_conn[p];
    return n;
}

/* エラー・相手が切った接続を張り直すよう頼む(転送していないときに呼ぶ)。 */
static void reconnect_down(void)
{
    for (unsigned p = 0; p < BFM_MAX_PORT; p++)
        for (unsigned i = 0; i < g_bfm_port_conn[p]; i++)
            if (bfm_conn_state(p, i) == BFM_ST_DOWN) (void)bfm_reconnect(p, i);
}

/* ---------------------------------------------------------------------
 * 自分の公開メモリ
 * ------------------------------------------------------------------- */

static void fill_area(void)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)g_bfm_mr_base;
    for (uint32_t i = 0; i < DATA_BYTES; i++) m[i] = pattern(i);
    for (uint32_t i = 0; i < ATOM_WORDS * 8u; i++) m[ATOM_OFF + i] = 0;
    *(volatile uint64_t *)(uintptr_t)(g_bfm_mr_base + SYNC_OFF) = 0;
}

static void dump_area(const char *who, const char *why)
{
    const volatile uint8_t *m = (const volatile uint8_t *)(uintptr_t)g_bfm_mr_base;
    printf("%s: [%s] 自分の公開メモリのデータ領域の hash = %016llx(相手の「模型の hash」と同じはず)\n", who, why,
           (unsigned long long)fnv1a(m, DATA_BYTES));
    for (unsigned w = 0; w < 3u; w++) {
        const volatile uint8_t *p = m + ATOM_OFF + w * 8u;
        uint64_t le = 0;
        for (int i = 7; i >= 0; i--) le = (le << 8) | p[i];
        printf("%s:   atomic 語 %u: %02x %02x %02x %02x %02x %02x %02x %02x(LE として %llu)\n", who, w,
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], (unsigned long long)le);
    }
}

/* 相手と足並みをそろえる: 相手の SYNC_OFF へ stage を書き、自分の SYNC_OFF が stage 以上になる
 * (相手もそこまで来た)まで bfm_run() を回して待つ。待つ間、切れた接続は張り直す。 */
static int sync_peer(bfm_conn_t *c, uint64_t stage, const char *what)
{
    volatile uint64_t *own = (volatile uint64_t *)(uintptr_t)(g_bfm_mr_base + SYNC_OFF);
    const int rc = bfm_write(c, SYNC_OFF, &stage, 8);
    if (rc != BFM_OK) {
        printf("run: 相手へ進み具合を書けない: %s\n", bfm_strerror(rc));
        return rc;
    }
    const uint64_t t0 = now_ns();
    while (*own < stage && !g_stop) {
        (void)bfm_run();
        reconnect_down();
        usleep(200);
        if (now_ns() - t0 > 600ull * 1000000000ull) {
            printf("run: 相手が「%s」まで来ない(10 分)\n", what);
            return BFM_E_TIMEOUT;
        }
    }
    return g_stop ? BFM_E_STATE : BFM_OK;
}

/* serve: 接続を張って保つ。切れた接続は張り直す。 */
static int run_serve(uint32_t nthreads)
{
    printf("serve: 接続を張って保つ(Ctrl-C で終わる、Ctrl-\\ で hash)\n");
    int rc = run_up(nthreads);
    while (!g_stop) {
        rc = bfm_run();
        reconnect_down();
        if (g_dump) { g_dump = 0; dump_area("serve", "要求"); }
        usleep(rc == BFM_OK ? 1000 : 50);
    }
    dump_area("serve", "終わり");
    return 0;
}

/* ---------------------------------------------------------------------
 * client
 * ------------------------------------------------------------------- */

static int g_ng;                     /* client の検査で NG になった数 */

static void check(const char *what, int ok)
{
    printf("run: %-58s %s\n", what, ok ? "OK" : "NG");
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
    printf("run: 遅延 %-9s x%u: 中央値 %u ns / 平均 %llu / 最小 %u / p99 %u / p99.9 %u\n", name, n,
           g_lat[n / 2u], (unsigned long long)(sum / n), g_lat[0], g_lat[(uint64_t)n * 99u / 100u],
           g_lat[(uint64_t)n * 999u / 1000u]);
}

static void run_dif(bfm_conn_t *c, uint32_t iters);
static void run_async(bfm_conn_t *c);

/* 接続 c で相手の公開メモリへ一通りの操作をして確かめる(相手も同時に、こちらへ同じことをしている)。 */
static int run_checks(bfm_conn_t *c, uint32_t iters)
{
    printf("run: 相手の公開メモリ %llu バイト、BlueFlame %s、inline %u バイトまで\n",
           (unsigned long long)bfm_remote_size(c), bfm_bf_active(c) ? "あり" : "なし", bfm_inline_max());
    if (bfm_remote_size(c) < SYNC_OFF + 8u) {
        printf("run: 相手の公開メモリが小さすぎる(%u バイト要る)\n", SYNC_OFF + 8u);
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
            if ((rc = bfm_read(c, off, buf, len)) != BFM_OK) { printf("run: read: %s\n", bfm_strerror(rc)); ok = 0; break; }
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
        printf("run: 相手のメモリ +16..+23: %02x %02x %02x %02x %02x %02x %02x %02x\n",
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
        printf("run: write_post 8B x%u: %.2f Mops\n", n, (double)n * 1000.0 / (double)dt);
        /* 測る間に書いたもの: 64 か所(64 バイトおき)が全部 g_src の先頭 64 バイトになっている。 */
        for (uint32_t k = 0; k < 64u; k++) memcpy(&g_model[k * 64u], g_src, 64);
        rc = bfm_read(c, 0, buf, DATA_BYTES);
        check("[6] 測った後のデータ領域が模型と一致", rc == BFM_OK && memcmp(buf, g_model, DATA_BYTES) == 0);
    }
    /* [8] T10-DIF(データ領域とは別の所を使う)。 */
    if (!g_stop) run_dif(c, iters);
    /* [9] 多重度つきの write / read(ASYNC_OFF から。データ領域とは別の所を使う)。 */
    if (!g_stop) run_async(c);
    printf("run: 結果 %s(NG %d 件)\n", g_ng ? "NG" : "OK", g_ng);
    return g_ng ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * [9] 多重度つきの write / read(bfm_write_async / bfm_read_async / bfm_test / bfm_check)
 *
 * 相手の公開メモリの ASYNC_OFF から ASYNC_N x 4KiB を使う(T10-DIF の領域と足並みの語の間)。
 * 相手の中身を手元でも持っておき(g_amodel)、読み戻して突き合わせる。
 * ------------------------------------------------------------------- */

#define ASYNC_OFF  (2u * 1024u * 1024u)
#define ASYNC_N    200u                    /* 多重度の上限(64)より多く出して、枠の刈り取りを通す */
#define ASYNC_SPAN 4096u

static uint8_t g_amodel[ASYNC_N * ASYNC_SPAN];   /* 相手の ASYNC_OFF からにあるはずの中身 */
static uint8_t g_aout[ASYNC_N * ASYNC_SPAN];     /* 読んだ先 */

static void run_async(bfm_conn_t *c)
{
    static bfm_tag_t tags[ASYNC_N], rtags[ASYNC_N];
    const uint32_t span = (g_bfm_async_buf_size < ASYNC_SPAN) ? g_bfm_async_buf_size : ASYNC_SPAN;
    uint32_t seed = 777u;
    int ok = 1, rc;
    printf("run: [9] 多重度 %u、枠の中継バッファ %u バイト、時間切れ %u ms\n", g_bfm_qdepth, g_bfm_async_buf_size,
           g_bfm_async_timeout_ms);
    /* ACK を確かめる write を 200 個出してから(64 個を超えると、刈り取って枠を空けながら出す)、tag ごとに確かめる */
    for (uint32_t k = 0; k < ASYNC_N && ok; k++) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t len = (k % 3u == 0u) ? 1u + k % bfm_inline_max() : 1u + (seed >> 8) % span;
        uint8_t *m = &g_amodel[k * ASYNC_SPAN];
        for (uint32_t j = 0; j < len; j++) m[j] = (uint8_t)((seed >> (j & 7u)) ^ j ^ k);
        if ((rc = bfm_write_async(c, ASYNC_OFF + k * ASYNC_SPAN, m, len, BFM_ACK, &tags[k])) != BFM_OK) {
            printf("run: [9] write_async: %s\n", bfm_strerror(rc));
            ok = 0;
        }
    }
    for (uint32_t k = 0; k < ASYNC_N && ok; k++)
        if (bfm_wait(c, tags[k]) != BFM_OK) ok = 0;
    check("[9] ACK を確かめる write 200 個(多重度を超えて出す)を tag ごとに確かめる", ok);
    /* read を 200 個出してから、tag ごとに確かめ、書いた中身と突き合わせる */
    memset(g_aout, 0xee, sizeof(g_aout));
    for (uint32_t k = 0; k < ASYNC_N && ok; k++)
        if (bfm_read_async(c, ASYNC_OFF + k * ASYNC_SPAN, &g_aout[k * ASYNC_SPAN], span, &rtags[k]) != BFM_OK) ok = 0;
    for (uint32_t k = 0; k < ASYNC_N && ok; k++)
        if (bfm_wait(c, rtags[k]) != BFM_OK) ok = 0;
    for (uint32_t k = 0; k < ASYNC_N && ok; k++)
        if (memcmp(&g_aout[k * ASYNC_SPAN], &g_amodel[k * ASYNC_SPAN], span) != 0) ok = 0;
    check("[9] read 200 個を tag ごとに確かめ、書いた中身と一致", ok);
    check("[9] 枠が次の操作に使われた古い tag は BFM_OK", bfm_test(c, tags[0]) == BFM_OK);
    /* write の直後に同じ所を read する(相手の NIC は順に処理するので、read はその write の中身を読む)。
     * 8 バイトの read は応答が CQE に入る(scatter to CQE)。 */
    for (uint32_t k = 0; k < 100u && ok; k++) {
        bfm_tag_t tw, tr;
        uint64_t v = 0x1111111111111111ull * (k + 1u) + k, r = 0;
        const uint64_t off = ASYNC_OFF + (k % 50u) * ASYNC_SPAN + 512u;
        if (bfm_write_async(c, off, &v, 8, BFM_ACK, &tw) != BFM_OK ||
            bfm_read_async(c, off, &r, 8, &tr) != BFM_OK || bfm_wait(c, tr) != BFM_OK ||
            bfm_test(c, tw) != BFM_OK || r != v)
            ok = 0;
        memcpy(&g_amodel[(k % 50u) * ASYNC_SPAN + 512u], &v, 8);
    }
    check("[9] write の直後の read はその write の中身を読む(両方の tag が BFM_OK)", ok);
    /* ACK を確かめない write を 2,000 個。新しく出さなくなった後、bfm_check() で仕掛かりが 0 になるのを見る */
    for (uint32_t k = 0; k < 2000u && ok; k++) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t len = 1u + k % 64u;
        const uint32_t at = (seed >> 4) % (ASYNC_N * ASYNC_SPAN - 64u);
        for (uint32_t j = 0; j < len; j++) g_amodel[at + j] = (uint8_t)(seed + j * 5u);
        if (bfm_write_async(c, ASYNC_OFF + at, &g_amodel[at], len, BFM_NOACK, NULL) != BFM_OK) ok = 0;
    }
    uint32_t inflight = 1;
    const uint64_t t0 = now_ns();
    while (ok && inflight != 0 && now_ns() - t0 < 1000000000ull)
        if (bfm_check(c, &inflight) != BFM_OK) ok = 0;
    check("[9] ACK を確かめない write 2,000 個の後、bfm_check() で仕掛かりが 0 になる", ok && inflight == 0);
    rc = bfm_read(c, ASYNC_OFF, g_aout, ASYNC_N * ASYNC_SPAN);
    check("[9] その後に読み戻すと中身が一致", ok && rc == BFM_OK && memcmp(g_aout, g_amodel, sizeof(g_amodel)) == 0);
    /* atomic: 同じ語へ 200 個(多重度を超えて)出しておく。相手の NIC は出した順に処理するので、FAA の元の値は
     * 0, 1, 2, ... と並び、i -> i + 1 の CAS は全部成功する(atomic 語 50〜53 を使う。最初は 0)。 */
    {
        static uint64_t old64[ASYNC_N];
        static uint32_t old32[ASYNC_N];
        int okf = 1, okc = 1, okf32 = 1, okc32 = 1;
        const uint64_t wf = ATOM_OFF + 8u * 50u, wc = ATOM_OFF + 8u * 51u, wf32 = ATOM_OFF + 8u * 52u,
                       wc32 = ATOM_OFF + 8u * 53u;
        uint64_t v = 0;
        uint32_t v32 = 0;
        for (uint32_t k = 0; k < ASYNC_N && okf; k++)
            if (bfm_faa_async(c, wf, 1, &old64[k], &tags[k]) != BFM_OK) okf = 0;
        for (uint32_t k = 0; k < ASYNC_N && okf; k++)
            if (bfm_wait(c, tags[k]) != BFM_OK || old64[k] != k) okf = 0;
        okf = okf && bfm_read(c, wf, &v, 8) == BFM_OK && v == ASYNC_N;
        check("[9] FAA 200 個を出しておくと、元の値が 0〜199 の順に並び、語は 200 になる", okf);
        for (uint32_t k = 0; k < ASYNC_N && okc; k++)
            if (bfm_cas_async(c, wc, k, k + 1u, &old64[k], &tags[k]) != BFM_OK) okc = 0;
        for (uint32_t k = 0; k < ASYNC_N && okc; k++)
            if (bfm_wait(c, tags[k]) != BFM_OK || old64[k] != k) okc = 0;
        okc = okc && bfm_read(c, wc, &v, 8) == BFM_OK && v == ASYNC_N;
        check("[9] CAS i->i+1 を 200 個出しておくと、全部成功する(元の値 = 比較値)", okc);
        for (uint32_t k = 0; k < ASYNC_N && okf32; k++)
            if (bfm_faa32_async(c, wf32, 1, &old32[k], &tags[k]) != BFM_OK) okf32 = 0;
        for (uint32_t k = 0; k < ASYNC_N && okf32; k++)
            if (bfm_wait(c, tags[k]) != BFM_OK || old32[k] != k) okf32 = 0;
        okf32 = okf32 && bfm_read(c, wf32, &v32, 4) == BFM_OK && v32 == ASYNC_N;
        check("[9] FAA32 200 個を出しておくと、元の値が 0〜199 の順に並ぶ", okf32);
        for (uint32_t k = 0; k < ASYNC_N && okc32; k++)
            if (bfm_cas32_async(c, wc32, k, k + 1u, &old32[k], &tags[k]) != BFM_OK) okc32 = 0;
        for (uint32_t k = 0; k < ASYNC_N && okc32; k++)
            if (bfm_wait(c, tags[k]) != BFM_OK || old32[k] != k) okc32 = 0;
        okc32 = okc32 && bfm_read(c, wc32, &v32, 4) == BFM_OK && v32 == ASYNC_N;
        check("[9] CAS32 i->i+1 を 200 個出しておくと、全部成功する", okc32);
        bfm_tag_t ta;
        check("[9] 境界の合わない非同期の atomic は BFM_E_PARAM",
              bfm_faa_async(c, wf + 4u, 1, NULL, &ta) == BFM_E_PARAM && bfm_cas32_async(c, wf32 + 2u, 0, 1, NULL, &ta) == BFM_E_PARAM);
    }
    bfm_tag_t t;
    check("[9] 枠の中継バッファより長い非同期の read は BFM_E_PARAM",
          bfm_read_async(c, ASYNC_OFF, g_aout, g_bfm_async_buf_size + 1u, &t) == BFM_E_PARAM);
}

/* ポート port の接続 i を張り直して、使えるまで待つ(相手も張り直す。30 秒まで)。 */
static bfm_conn_t *reconnect_wait(unsigned port, unsigned i)
{
    for (int k = 0; k < 100 && bfm_conn_state(port, i) == BFM_ST_UP && bfm_conn(port, i) == NULL; k++) {
        (void)bfm_run();
        idle();
    }
    if (bfm_reconnect(port, i) != BFM_OK) return NULL;
    const uint64_t t0 = now_ns();
    while (bfm_conn(port, i) == NULL && !g_stop && now_ns() - t0 < 30ull * 1000000000ull) {
        (void)bfm_run();
        idle();
    }
    return bfm_conn(port, i);
}

/* [7] 範囲の外。相手の NIC が REMOTE_ACCESS で断ること(相手の MKey が範囲を持っていること)。
 * 接続はエラーになるので、bfm_reconnect() で張り直し、張り直した接続で読めることも確かめる
 * (相手は BYE を受けて、自分の側を張り直す)。 */
static void run_oob(bfm_conn_t *c, unsigned port, unsigned i)
{
    struct bfm_conn *ci = (struct bfm_conn *)c;
    const uint64_t real = ci->rlen;
    ci->rlen = real + 4096u;                  /* 手元の範囲検査だけをすり抜ける */
    uint8_t d[8];
    int rc = bfm_read(c, real, d, 8);
    ci->rlen = real;
    check("[7] 公開メモリの外を読むと相手が断る(REMOTE_ACCESS)", rc == BFM_E_REMOTE);
    for (int k = 0; k < 100 && bfm_conn_state(port, i) != BFM_ST_DOWN; k++) {   /* bfm_run() が K_DOWN に移す */
        (void)bfm_run();
        idle();
    }
    check("[7] 断られた接続は BFM_ST_DOWN になる", bfm_conn_state(port, i) == BFM_ST_DOWN);
    rc = bfm_reconnect(port, i);
    const uint64_t t0 = now_ns();
    while (rc == BFM_OK && bfm_conn(port, i) == NULL && !g_stop && now_ns() - t0 < 30ull * 1000000000ull) {
        (void)bfm_run();
        idle();
    }
    bfm_conn_t *c2 = bfm_conn(port, i);
    uint64_t v = 0;
    check("[7] bfm_reconnect() で張り直して、読める", rc == BFM_OK && c2 != NULL &&
          bfm_read(c2, ATOM_OFF + 8u, &v, 8) == BFM_OK);
    if (c2 == NULL) return;

    /* 張り直しの前の tag(成功した ACK あり write のもの)。張り直した後は BFM_E_STATE(結果は分からない)になる */
    bfm_tag_t old_tag = 0;
    const int old_ok = bfm_write_async(c2, ATOM_OFF + 64u, &v, 8, BFM_ACK, &old_tag) == BFM_OK &&
                       bfm_wait(c2, old_tag) == BFM_OK;

    /* ACK を確かめない write の失敗は、bfm_check() で分かる */
    ci = (struct bfm_conn *)c2;
    ci->rlen = real + 4096u;
    rc = bfm_write_async(c2, real, &v, 8, BFM_NOACK, NULL);
    ci->rlen = real;
    const uint64_t t1 = now_ns();
    while (rc == BFM_OK && now_ns() - t1 < 1000000000ull) rc = bfm_check(c2, NULL);
    check("[7] 公開メモリの外への ACK を確かめない write の失敗を bfm_check() が返す(REMOTE_ACCESS)", rc == BFM_E_REMOTE);
    bfm_conn_t *c3 = reconnect_wait(port, i);
    check("[7] 張り直した", c3 != NULL);
    if (c3 == NULL) return;
    check("[7] 張り直しの前に出した操作の tag は、張り直した後は BFM_E_STATE",
          old_ok && bfm_test(c3, old_tag) == BFM_E_STATE);

    /* 時間切れ: 時間切れを 0ms にして、ACK が返る前に bfm_check() すると BFM_E_TIMEOUT になる */
    const uint32_t save = g_bfm_async_timeout_ms;
    g_bfm_async_timeout_ms = 0;
    rc = BFM_OK;
    for (int k = 0; k < 100 && rc == BFM_OK; k++) {
        rc = bfm_write_async(c3, ATOM_OFF + 64u, &v, 8, BFM_NOACK, NULL);
        if (rc == BFM_OK) rc = bfm_check(c3, NULL);
    }
    g_bfm_async_timeout_ms = save;
    check("[7] 出してから時間切れを過ぎても ACK が無ければ bfm_check() が BFM_E_TIMEOUT(接続はエラー)",
          rc == BFM_E_TIMEOUT && !bfm_is_up(c3));
    bfm_conn_t *c4 = reconnect_wait(port, i);
    check("[7] 時間切れの後も張り直して、読める", c4 != NULL && bfm_read(c4, ATOM_OFF + 8u, &v, 8) == BFM_OK);
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
            printf("run: DIF のスループット: %s(rc=%d)\n", bfm_strerror(rc), rc);
            g_ng++;
            return;
        }
    }
    const double ns = (double)(now_ns() - t0);
    static const char *ops[] = {"CHECK", "CHECK_SWAP", "INSERT", "CHECK_DELETE"};
    printf("run: スループット DIF %s %-5s %-12s %u ブロック x%u: %.1f MB/s(1 回 %.1f us)\n",
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
            printf("run: DIF の遅延: %s(rc=%d)\n", bfm_strerror(rc), rc);
            g_ng++;
            return;
        }
        if (i >= 100u) g_lat[i - 100u] = (uint32_t)(now_ns() - t0);
    }
    qsort(g_lat, n, sizeof(g_lat[0]), cmp_u32);
    static const char *ops[] = {"CHECK", "CHECK_SWAP", "INSERT", "CHECK_DELETE"};
    printf("run: 遅延 DIF %s %-5s %-12s 4KiB x%u: 中央値 %u ns / p99 %u\n", mode == BFM_DIF_HW ? "HW" : "SW",
           is_read ? "read" : "write", ops[op], n, g_lat[n / 2u], g_lat[(uint64_t)n * 99u / 100u]);
}

static void run_dif(bfm_conn_t *c, uint32_t iters)
{
    if (bfm_remote_size(c) < DIF_R2 + DIF_N * DIF_BS) {
        printf("run: [8] 相手の公開メモリが小さいので T10-DIF の検査を飛ばす\n");
        return;
    }
    printf("run: [8] T10-DIF(%u ブロック)。オフロード %s\n", DIF_N,
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
 * 張ってある全部の接続(全部のポート)に 1 本ずつスレッドを付け、相手の公開メモリの
 * MT_OFF + スレッドの番号 x MT_SPAN(スレッドごとに別の区画)で次を確かめる。相手も同時に、
 * こちらの公開メモリへ同じことをしている(両方向)。
 *   [1] 4KiB の write(中継バッファから)と read で読み戻す(64 回)
 *   [2] 8 バイトの write と read で読み戻す
 *   [3] 全スレッドが同じ語へ FAA を n 回ずつ打つ -> 最後に n x スレッド数になる(接続をまたいだ atomic)
 *   [4] T10-DIF(SW)の write INSERT と read CHECK_DELETE で元のデータに戻る(起動と完了確認を分けた形)
 * そのあと全スレッドで同時に、8 バイトの write / read / FAA(自分の語)を n 回ずつ打ち、合計の速さを出す。
 * 間ずっと別のスレッドが bfm_run() を回す(鍵を取る側も同時に動かす)。
 * スレッドは CPU に 1 つずつ固定する(CPU の数より多ければ重ねる)。
 * ------------------------------------------------------------------- */

#define MT_OFF     (4u * 1024u * 1024u)
#define MT_SPAN    (64u * 1024u)
#define MT_CNT_OFF (MT_OFF + BFM_MAX_CONN * MT_SPAN)   /* 全スレッドで共有する FAA の語 */
#define MT_DIF_N   16u
#define MT_PH      13u                    /* 速さの区間の数(g_mt_ph)*/
#define MT_SYNC    100u                   /* 速さの区間ごとに相手と足並みをそろえる段の番号の起点(区間の数だけ使う。200 未満)*/
_Static_assert(MT_SYNC + MT_PH <= 200u, "速さの区間の段の番号が、全部の接続での転送の段(200)と重ならないこと");
#define MT_BIG     (64u * 1024u)          /* 帯域を測る write / read の長さ */

/* 速さの区間。0〜4 は 1 回ずつ完了を待つ関数、5〜12 は多重度つき(bfm_*_async。g_bfm_qdepth 個まで出しておく)。
 * 回数 = -n / div。bytes が 4KiB 以上の区間は MB/s で、それ以外は Mops で出す。 */
static const struct {
    const char *name;                     /* 区間の名前(表示と相手との足並み)*/
    uint32_t div;                         /* 回数 = -n / div */
    uint32_t bytes;                       /* 1 回の長さ */
} g_mt_ph[MT_PH] = {
    {"write 8B", 1u, 8u}, {"read 8B", 1u, 8u}, {"FAA", 1u, 8u},
    {"write 64KiB", 50u, MT_BIG}, {"read 64KiB", 50u, MT_BIG},
    {"aw 8B NOACK", 1u, 8u}, {"aw 8B ACK", 1u, 8u}, {"ar 8B", 1u, 8u},
    {"aw 4KiB ACK", 10u, 4096u}, {"ar 4KiB", 10u, 4096u},
    {"aFAA", 1u, 8u}, {"aCAS", 1u, 8u}, {"aFAA 64語", 1u, 8u},
};
static uint8_t g_mt_big[MT_BIG];          /* 帯域を測る write の元(どのスレッドも読むだけ)*/
static __thread uint8_t mt_rbuf[MT_BIG];  /* 帯域を測る read の着地点(スレッドごと)*/
static __thread uint64_t mt_r8[64];       /* 多重度つきの 8 バイトの read の着地点(スレッドごと)*/

/* 区間 ph の回数(1 本のスレッドあたり)。 */
static uint32_t mt_reps(unsigned ph, uint32_t n)
{
    return (n / g_mt_ph[ph].div) ? n / g_mt_ph[ph].div : 1u;
}

/* 多重度つきの区間を 1 回分打つ(区間 5〜12。aFAA / aCAS は同じ 1 つの語へ、aFAA 64語 は 64 バイトおきの 64 個の語へ順に)。最後に出したものが済むまで待つ(完了は出した順に来るので、
 * それで全部済んでいる)。ACK を確かめない write は bfm_check() で仕掛かりが 0 になるまで見る。 */
static int mt_async(bfm_conn_t *c, unsigned ph, uint64_t own, uint64_t big, uint32_t reps)
{
    bfm_tag_t tag = 0;
    int rc = BFM_OK;
    for (uint32_t k = 0; k < reps && rc == BFM_OK; k++) {
        uint64_t v = k;
        switch (ph) {
        case 5: rc = bfm_write_async(c, own, &v, 8, BFM_NOACK, NULL); break;
        case 6: rc = bfm_write_async(c, own, &v, 8, BFM_ACK, &tag); break;
        case 7: rc = bfm_read_async(c, own, &mt_r8[k & 63u], 8, &tag); break;
        case 8: rc = bfm_write_async(c, big + (k & 15u) * 4096u, g_mt_big, 4096, BFM_ACK, &tag); break;
        case 10: rc = bfm_faa_async(c, own + 24u, 1, &mt_r8[k & 63u], &tag); break;
        case 11: rc = bfm_cas_async(c, own + 32u, k, k + 1u, &mt_r8[k & 63u], &tag); break;
        case 12: rc = bfm_faa_async(c, big + (k & 63u) * 64u, 1, &mt_r8[k & 63u], &tag); break;
        default: rc = bfm_read_async(c, big + (k & 15u) * 4096u, mt_rbuf + (k & 15u) * 4096u, 4096, &tag); break;
        }
    }
    if (rc != BFM_OK) return rc;
    if (ph != 5) return bfm_wait(c, tag);
    uint32_t inflight = 1;
    while (rc == BFM_OK && inflight != 0) rc = bfm_check(c, &inflight);
    return rc;
}

static int sync_at(uint64_t stage, const char *what);

struct mt_arg {
    pthread_t th;                        /* このスレッド */
    int idx;                             /* スレッドの番号(区画の位置と CPU を決める)*/
    bfm_conn_t *c;                       /* このスレッドの接続(NULL = 使えない)*/
    int ng;                              /* NG の数 */
    char why[200];                       /* 最初の NG の内容 */
    uint64_t ns[MT_PH];                  /* 速さの区間ごとにかかった時間 */
};

static pthread_barrier_t g_mt_bar;       /* 全スレッドで足並みをそろえる(同時に接続・測る・切断する)*/
static char g_mt_msg[200];               /* 結果の行を組み立てる置き場 */
static volatile int g_mt_poll_stop;      /* 1 = bfm_run() を回すスレッドを止める */
static uint32_t g_mt_n;                  /* 1 本のスレッドが速さの区間で打つ回数(-n)*/
static uint32_t g_mt_threads;            /* スレッドの数(= 接続の数)*/
static uint64_t g_mt_counter;            /* [3] の結果(スレッド 0 が読む)*/

static void mt_fail(struct mt_arg *a, const char *what, int rc)
{
    if (a->ng++ == 0) snprintf(a->why, sizeof(a->why), "%s(rc=%d %s)", what, rc, bfm_strerror(rc));
}

static void *mt_poller(void *p)
{
    (void)p;
    while (!g_mt_poll_stop) {
        (void)bfm_run();
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

    pthread_barrier_wait(&g_mt_bar);                         /* 全スレッドで同時に始める */
    if (a->c == NULL) mt_fail(a, "接続が使えない", 0);
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

    /* 速さ: 区間ごとに相手と足並みをそろえ(スレッド 0 が)、全スレッドで同時に打つ。
     * 相手も同じ区間を同時に打つ(-q の相手は打たずに足並みだけそろえる)ので、両方向の速さになる。 */
    const uint64_t own = MT_OFF + (uint64_t)a->idx * MT_SPAN + 4096u + 512u;
    const uint64_t big = MT_OFF + (uint64_t)a->idx * MT_SPAN;
    for (unsigned ph = 0; ph < MT_PH; ph++) {
        if (a->idx == 0 && sync_at(MT_SYNC + ph, g_mt_ph[ph].name) != BFM_OK) mt_fail(a, "相手と足並みをそろえられない", 0);
        pthread_barrier_wait(&g_mt_bar);
        const uint64_t t0 = now_ns();
        const uint32_t reps = mt_reps(ph, g_mt_n);
        if (ph >= 5) {
            if (a->c && !a->ng && (rc = mt_async(a->c, ph, own, big, reps)) != BFM_OK) mt_fail(a, g_mt_ph[ph].name, rc);
            a->ns[ph] = now_ns() - t0;
            continue;
        }
        for (uint32_t k = 0; k < reps && a->c && !a->ng; k++) {
            uint64_t v = k, old;
            rc = (ph == 0) ? bfm_write(a->c, own, &v, 8) : (ph == 1) ? bfm_read(a->c, own, &v, 8)
               : (ph == 2) ? bfm_faa(a->c, own + 8u, 1, &old)
               : (ph == 3) ? bfm_write(a->c, big, g_mt_big, MT_BIG) : bfm_read(a->c, big, mt_rbuf, MT_BIG);
            if (rc != BFM_OK) mt_fail(a, g_mt_ph[ph].name, rc);
        }
        a->ns[ph] = now_ns() - t0;
    }
    pthread_barrier_wait(&g_mt_bar);
    return NULL;
}

static int run_mt(uint32_t n)
{
    static struct mt_arg args[BFM_MAX_CONN];
    uint32_t nthreads = 0;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++)
        for (unsigned i = 0; i < g_bfm_port_conn[p]; i++) {
            memset(&args[nthreads], 0, sizeof(args[nthreads]));
            args[nthreads].idx = (int)nthreads;
            args[nthreads].c = bfm_conn(p, i);
            nthreads++;
        }
    if (nthreads == 0) return 2;
    g_mt_n = n;
    g_mt_threads = nthreads;
    g_mt_poll_stop = 0;
    printf("mt: 接続 %u 本(全部のポート)に 1 本ずつスレッド(CPU %ld 個)、各 %u 回。相手も同時にこちらへ打つ\n", nthreads,
           sysconf(_SC_NPROCESSORS_ONLN), n);
    pthread_barrier_init(&g_mt_bar, NULL, nthreads);
    pthread_t poller;
    pthread_create(&poller, NULL, mt_poller, NULL);
    for (uint32_t t = 0; t < nthreads; t++) {
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
    snprintf(g_mt_msg, sizeof(g_mt_msg), "%u 本の接続が全部使える(%u 本)", nthreads, up);
    check(g_mt_msg, up == nthreads);
    snprintf(g_mt_msg, sizeof(g_mt_msg), "[1][2][4] スレッドごとの write / read / T10-DIF(NG のスレッド %u 本)", ng);
    check(g_mt_msg, ng == 0);
    snprintf(g_mt_msg, sizeof(g_mt_msg), "[3] 共有の語への FAA の合計 = %llu(あるべき %llu)",
             (unsigned long long)g_mt_counter, (unsigned long long)n * nthreads);
    check(g_mt_msg, g_mt_counter == (uint64_t)n * nthreads);
    if (ng == 0) {
        for (unsigned ph = 0; ph < MT_PH; ph++) {
            uint64_t worst = 0, sum = 0;
            for (uint32_t t = 0; t < nthreads; t++) {
                if (args[t].ns[ph] > worst) worst = args[t].ns[ph];
                sum += args[t].ns[ph];
            }
            const uint32_t reps = mt_reps(ph, n);
            if (g_mt_ph[ph].bytes >= 4096u) {
                const double mb = (double)reps * g_mt_ph[ph].bytes * nthreads / ((double)worst / 1e9) / 1e6;
                printf("mt: 速さ %-11s スレッド %2u 本: 合計 %.0f MB/s(%.2f Gbit/s)\n", g_mt_ph[ph].name, nthreads, mb,
                       mb * 8.0 / 1000.0);
                continue;
            }
            printf("mt: 速さ %-11s スレッド %2u 本: 合計 %.2f Mops(1 本あたり %.2f Mops、1 回 %.0f ns)\n", g_mt_ph[ph].name,
                   nthreads, (double)reps * nthreads / (double)worst * 1000.0,
                   (double)reps / ((double)sum / nthreads) * 1000.0, (double)sum / nthreads / reps);
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

static int run_bench(uint32_t nthreads, uint32_t n)
{
    static const uint32_t sizes[] = { 4, 8, 32, 64, 128, 256, 512, 8192, 65536, 262144 };
    static const uint32_t qds[] = { 1, 4, 8, 16 };
    if (run_up(nthreads) != BFM_OK) return 1;
    bfm_conn_t *c = bfm_conn(0, 0);
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
    return ng;
}

/* ---------------------------------------------------------------------
 * run: 両方のノードで同じものを動かす
 * ------------------------------------------------------------------- */

/* 最初に使うポート(足並みをそろえる語と一通りの検査は、そのポートの接続 0 を使う)。 */
static unsigned first_port(void)
{
    for (unsigned p = 0; p < BFM_MAX_PORT; p++)
        if (g_bfm_port_conn[p] != 0) return p;
    return 0;
}

/* 相手と足並みをそろえる(接続 (p0, 0) が使えるまで待ってから)。 */
static int sync_at(uint64_t stage, const char *what)
{
    const unsigned p0 = first_port();
    const uint64_t t0 = now_ns();
    for (;;) {
        bfm_conn_t *c = bfm_conn(p0, 0);
        if (c != NULL && sync_peer(c, stage, what) == BFM_OK) {
            printf("run: 相手も「%s」まで来た\n", what);
            return BFM_OK;
        }
        if (g_stop || now_ns() - t0 > 60ull * 1000000000ull) {
            printf("run: 相手と「%s」で足並みをそろえられない\n", what);
            return BFM_E_TIMEOUT;
        }
        (void)bfm_run();
        reconnect_down();
        usleep(1000);
    }
}

/* ---------------------------------------------------------------------
 * [10] 無応答(-z。両方のノードに付ける)
 *
 * 黙る側(IP の大きいほう)が、ポート p0 の最後の 4 本の接続の QP を、BYE を送らずに消す(張り直しを頼んだ扱いにして
 * 受け側の確立の最初 = REQ 待ちへ戻す)。消された QP 宛てのパケットは相手の NIC が黙って捨てるので、試す側
 * (IP の小さいほう)には ACK も応答も返らない。試す側はその 4 本で次を確かめ、最後に張り直す。
 *   A  ACK を確かめる write で枠を全部(既定 64 個)埋め、次の 1 個が資源の空きを待って、最初を出してから
 *      時間切れ(既定 100ms)で BFM_E_TIMEOUT(待った時間は、ライブラリと同じく最初の操作を出した時刻から測る)
 *   B  ACK を確かめない write を 10 個出し、bfm_check() が 100ms で BFM_E_TIMEOUT を報告する
 *   C  read を 1 個出し、bfm_wait() が 100ms で BFM_E_TIMEOUT を返す
 *   D  1 回ずつ待つ bfm_write() を出し、NIC が ack timeout ごとに送り直して、尽きると BFM_E_RETRY を返す。
 *      かかった時間から、NIC が実際に待った ack timeout を求めて表示する(設定以上か。ConnectX-4 Lx は約 0.37 秒以上)
 * ------------------------------------------------------------------- */

/* 待った時間が時間切れ(g_bfm_async_timeout_ms)から、その 5 倍までの間か。 */
static int near_timeout(uint64_t ns)
{
    const uint64_t lim = (uint64_t)g_bfm_async_timeout_ms * 1000000u;
    return ns >= lim && ns <= lim * 5u;
}

#define SILENT_N 4u                       /* 黙らせる接続の数(A・B・C・D に 1 本ずつ)*/

static int run_silent(void)
{
    const unsigned p = first_port(), n = g_bfm_port_conn[p];
    const int tester = g_bfm_local_ip[p] < g_bfm_peer_ip[p];
    if (n < 5u) {
        printf("run: [10] 無応答の試験は、ポートの接続が 5 本以上のときだけ行う(%u 本)\n", n);
        return BFM_OK;
    }
    if (!tester) {
        /* 黙る側: 最後の 4 本の QP を、BYE を送らずに消す(状態を「相手が切った」にしてから張り直しを頼むと、
         * BYE を送らずに QP と CQ を消し、受け側の確立の最初 = REQ 待ちに戻る) */
        for (unsigned j = 0; j < SILENT_N; j++) {
            struct bfm_conn *ci = &g_bfm_conn[p * BFM_MAX_PORT_CONN + n - 1u - j];
            ci->state = BFM_CS_CLOSED;
            (void)bfm_reconnect(p, n - 1u - j);
        }
        const uint64_t t0 = now_ns();
        int gone = 0;
        while (!gone && now_ns() - t0 < 5000000000ull) {
            (void)bfm_run();
            gone = 1;
            for (unsigned j = 0; j < SILENT_N; j++)
                if (g_bfm_conn[p * BFM_MAX_PORT_CONN + n - 1u - j].qp.has_qp) gone = 0;
            idle();
        }
        printf("run: [10] 黙る側: ポート %u の接続 %u〜%u の QP を、BYE を送らずに消した(%s)\n", p, n - SILENT_N, n - 1u,
               gone ? "済んだ" : "消えない");
        return sync_at(210, "無応答の用意");
    }

    /* 試す側 */
    if (sync_at(210, "無応答の用意") != BFM_OK) return BFM_E_TIMEOUT;
    printf("run: [10] 試す側: 相手はポート %u の接続 %u〜%u の QP を消した(ACK も応答も返らない)。時間切れ %u ms\n", p,
           n - SILENT_N, n - 1u, g_bfm_async_timeout_ms);
    uint64_t v = 0x5a5a5a5a5a5a5a5aull;
    int rc;

    /* A: 枠を全部埋めて、65 個目が資源の空きを待つ */
    {
        bfm_conn_t *c = bfm_conn(p, n - 1u);
        bfm_tag_t tags[BFM_QD_MAX], t65 = 0;
        int ok = (c != NULL);
        const uint32_t qd = ok ? ((struct bfm_conn *)c)->aqd : 0u;   /* ライブラリが実際に使う枠の数 */
        const uint64_t t0 = now_ns();          /* 時間切れの起点 = 最初の操作を出した時刻 */
        for (uint32_t k = 0; k < qd && ok; k++)
            if (bfm_write_async(c, ATOM_OFF + 64u + (k & 7u) * 8u, &v, 8, BFM_ACK, &tags[k]) != BFM_OK) ok = 0;
        uint32_t inflight = 0;
        if (ok) (void)bfm_check(c, &inflight);
        snprintf(g_mt_msg, sizeof(g_mt_msg), "[10] A: ACK の返らない write で枠が全部(%u 個)仕掛かり中になる", qd);
        check(g_mt_msg, ok && inflight == qd);
        rc = ok ? bfm_write_async(c, ATOM_OFF + 64u, &v, 8, BFM_ACK, &t65) : BFM_E_STATE;
        const uint64_t dt = now_ns() - t0;
        printf("run: [10] A: %u 個目は資源の空きを待ち、最初を出してから %.1f ms で %s\n", qd + 1u, (double)dt / 1e6,
               bfm_strerror(rc));
        check("[10] A: 枠が埋まった後の 1 個は資源の空きを待ち、時間切れで BFM_E_TIMEOUT", rc == BFM_E_TIMEOUT && near_timeout(dt));
        check("[10] A: 接続はエラーになり、仕掛かり中の tag も BFM_E_TIMEOUT", ok && !bfm_is_up(c) &&
              bfm_test(c, tags[0]) == BFM_E_TIMEOUT);
    }
    /* B: ACK を確かめない write の時間切れを bfm_check() で知る */
    {
        bfm_conn_t *c = bfm_conn(p, n - 2u);
        int ok = (c != NULL);
        const uint64_t t0 = now_ns();          /* 時間切れの起点 = 最初の操作を出した時刻 */
        for (uint32_t k = 0; k < 10u && ok; k++)
            if (bfm_write_async(c, ATOM_OFF + 64u, &v, 8, BFM_NOACK, NULL) != BFM_OK) ok = 0;
        uint32_t inflight = 0;
        rc = BFM_OK;
        while (ok && rc == BFM_OK && now_ns() - t0 < 2000000000ull) rc = bfm_check(c, &inflight);
        const uint64_t dt = now_ns() - t0;
        printf("run: [10] B: bfm_check() は %.1f ms で %s(仕掛かり %u 個)\n", (double)dt / 1e6, bfm_strerror(rc), inflight);
        check("[10] B: ACK の返らない ACK なし write を、bfm_check() が時間切れで報告する",
              ok && rc == BFM_E_TIMEOUT && inflight == 10u && near_timeout(dt));
    }
    /* C: read の応答が来ない */
    {
        bfm_conn_t *c = bfm_conn(p, n - 3u);
        bfm_tag_t t = 0;
        uint64_t r = 0;
        const uint64_t t0 = now_ns();
        rc = (c != NULL) ? bfm_read_async(c, ATOM_OFF + 8u, &r, 8, &t) : BFM_E_STATE;
        if (rc == BFM_OK) rc = bfm_wait(c, t);
        const uint64_t dt = now_ns() - t0;
        printf("run: [10] C: read の bfm_wait() は %.1f ms で %s\n", (double)dt / 1e6, bfm_strerror(rc));
        check("[10] C: 応答の返らない read を、bfm_wait() が時間切れで返す", rc == BFM_E_TIMEOUT && near_timeout(dt));
    }
    /* D: NIC の再送の間隔。1 回ずつ待つ bfm_write()(時間切れ g_bfm_op_timeout_ms = 5 秒)を出すと、NIC は
     * ack timeout ごとに g_bfm_retry_count 回送り直してから TRANSPORT_RETRY_EXC を返す(BFM_E_RETRY)。
     * かかった時間 / (再送回数 + 1) が、NIC が実際に待った ack timeout */
    {
        bfm_conn_t *c = bfm_conn(p, n - 4u);
        const uint64_t t0 = now_ns();
        rc = (c != NULL) ? bfm_write(c, ATOM_OFF + 64u, &v, 8) : BFM_E_STATE;
        const uint64_t dt = now_ns() - t0;
        const double set_ms = 4.096e-3 * (double)(1u << g_bfm_ack_timeout);
        const double per_ms = (double)dt / 1e6 / (double)(g_bfm_retry_count + 1u);
        printf("run: [10] D: bfm_write() は %.1f ms で %s(ack timeout の設定 %u = %.1f ms、再送 %u 回 → 1 回あたり %.1f ms)\n",
               (double)dt / 1e6, bfm_strerror(rc), g_bfm_ack_timeout, set_ms, g_bfm_retry_count, per_ms);
        check("[10] D: ACK の返らない write を NIC が送り直し、尽きると BFM_E_RETRY(TRANSPORT_RETRY_EXC)",
              rc == BFM_E_RETRY);
        /* 上限は見ない(ConnectX-4 Lx は設定によらず約 0.37〜0.40 秒待つ。下限の値は装置で違う)*/
        snprintf(g_mt_msg, sizeof(g_mt_msg), "[10] D: 再送の間隔が ack timeout の設定(%.1f ms)以上", set_ms);
        check(g_mt_msg, rc == BFM_E_RETRY && per_ms >= set_ms);
    }
    /* 張り直す(相手の黙らせた側は REQ を待っている)*/
    for (unsigned j = 0; j < SILENT_N; j++) {
        uint64_t r = 0;
        bfm_conn_t *c = reconnect_wait(p, n - 1u - j);
        snprintf(g_mt_msg, sizeof(g_mt_msg), "[10] 接続 %u を張り直して、読める", n - 1u - j);
        check(g_mt_msg, c != NULL && bfm_read(c, ATOM_OFF + 8u, &r, 8) == BFM_OK);
    }
    return BFM_OK;
}

static int run_all(uint32_t nthreads, uint32_t iters, int oob)
{
    if (run_up(nthreads) != BFM_OK) return 1;
    const unsigned p0 = first_port();
    int ret = 0;

    /* 一通りの検査(接続 (p0, 0)。相手も同時にこちらへ同じことをする)。-q なら相手からを受けるだけ */
    if (!g_quiet && !g_perf) ret |= run_checks(bfm_conn(p0, 0), iters ? iters : 20000u);
    if (g_stop || sync_at(1, "一通りの検査") != BFM_OK) return 1;

    /* 全部の接続で同時に(相手も同時に)。-q なら速さの区間の足並みだけそろえる */
    g_ng = 0;
    if (!g_quiet) {
        ret |= run_mt(iters ? iters : 100000u);
    } else {
        for (unsigned ph = 0; ph < MT_PH && !g_stop; ph++)
            if (sync_at(MT_SYNC + ph, g_mt_ph[ph].name) != BFM_OK) return 1;
    }
    if (g_stop || sync_at(200, "全部の接続での転送") != BFM_OK) return 1;

    /* 無応答(-z。両方のノードに付ける)*/
    if (g_silent) {
        g_ng = 0;
        if (run_silent() != BFM_OK) return 1;
        ret |= g_ng ? 1 : 0;
    }

    /* 範囲の外と張り直し(最後の接続。相手は待つ間に自分の側を張り直す)*/
    if (oob && !g_quiet) {
        unsigned p = first_port(), i = g_bfm_port_conn[p] - 1u;
        for (unsigned q = 0; q < BFM_MAX_PORT; q++)
            if (g_bfm_port_conn[q] != 0) { p = q; i = g_bfm_port_conn[q] - 1u; }
        g_ng = 0;
        bfm_conn_t *c = bfm_conn(p, i);
        if (c != NULL) run_oob(c, p, i);
        else check("[7] 最後の接続が使える", 0);
        ret |= g_ng ? 1 : 0;
    }
    if (g_stop || sync_at(300, "終わり") != BFM_OK) return 1;

    if (!g_quiet)
        printf("run: 模型の hash = %016llx(相手が出す「自分の公開メモリの hash」と同じはず)\n",
               (unsigned long long)fnv1a(g_model, DATA_BYTES));
    dump_area("run", "終わり");
    printf("run: 結果 %s\n", ret ? "NG" : "OK");
    return ret;
}

/* ---------------------------------------------------------------------
 * race: 複数のコアから同時に動かして、競合を確かめる(両方のノードで race を動かす)
 *
 *   段 1 張る:       -t 本のスレッドが bfm_run() を同時に呼んで全部の接続を張る(run と同じ)
 *   段 2 揺さぶる:   -n 秒(既定 30)、次を同時に動かす
 *     - bfm_run() を休まず呼ぶスレッド -t 本(張り直しの途中の接続を進める)
 *     - データの接続(各ポートの前半)に 1 本ずつ: 4KiB の write / read と、多重度つきの 8 バイトの write
 *       16 個 + read で読み戻して確かめ続ける。一度も失敗しないこと(隣の接続の張り直しに巻き込まれない)
 *     - 張り直しの接続(各ポートの後半)に 1 本ずつ: 使えれば 8 バイトの write / read で確かめてから
 *       bfm_reconnect()、切れていれば bfm_reconnect() を繰り返す(相手も同時に同じことをする)。
 *       write と read が両方通ったのに読み戻した値が違うことが無いこと
 *     - 見張り 1 本: 全部の接続の bfm_conn_state() を読み続け、2 秒ごとに張り直しの接続 1 本の bfm_conn_steplog()
 *     bfm_run() が、失敗した接続(BFM_E_STATE 以外の負)を一度も返さないこと
 *   段 3 そろえる:   張り直しをやめ、全部の接続が使えるようになること
 *   段 4 止めて立ち上げ直す: bfm_run() と reconnect_down() を回すスレッドを動かしたまま bfm_shutdown() を呼び、
 *     止まること。止まると、回しているスレッドが R_IDLE から立ち上げ直しを取り合う(1 回だけ行われる)。
 *     全部の接続が使えるようになり、全部の接続で 8 バイトの write / read が通ること
 * ------------------------------------------------------------------- */

#define RACE_RUNNERS_MAX 64u

struct race_runner {
    pthread_t th;
    uint64_t calls, ok, inprog, estate, fail;   /* bfm_run() の回数と戻り値ごとの数 */
    int first_fail;                              /* 最初に見た失敗の値 */
};

struct race_conn {
    pthread_t th;
    unsigned port, i, idx;               /* ポート・ポートの中の番号・全体の通し番号(区画の位置)*/
    int churn;                           /* 1 = 張り直しの接続、0 = データの接続 */
    uint64_t ops, errs, cycles;          /* 確かめた回数・失敗した操作の数・張り直しを頼んだ回数 */
    int ng;                              /* 1 = 本当の誤り(データの接続の失敗・読み戻しの食い違い・FAILED)*/
    char why[160];                       /* 最初の本当の誤り */
};

static volatile int g_race_stop_runner;  /* 1 = bfm_run() を回すスレッドを止める */
static volatile int g_race_stop_work;    /* 1 = データ・張り直し・見張りのスレッドを止める */
static volatile int g_race_reconnect;    /* 1 = 回すスレッドが、切れた接続の張り直しも頼む(段 4)*/
static uint64_t g_race_watch_reads;      /* 見張りが読んだ状態の数 */

static void race_ng(struct race_conn *a, const char *what, int rc)
{
    if (a->ng++ == 0) snprintf(a->why, sizeof(a->why), "%s(rc=%d %s)", what, rc, bfm_strerror(rc));
}

static void *race_runner_fn(void *p)
{
    struct race_runner *a = (struct race_runner *)p;
    while (!g_race_stop_runner && !g_stop) {
        const int rc = bfm_run();
        a->calls++;
        if (rc == BFM_OK) a->ok++;
        else if (rc == BFM_INPROGRESS) a->inprog++;
        else if (rc == BFM_E_STATE) {
            a->estate++;
            if (g_race_reconnect) reconnect_down();
        } else if (a->fail++ == 0) a->first_fail = rc;
    }
    return NULL;
}

/* データの接続: 一度も失敗しないこと。 */
static void race_data(struct race_conn *a)
{
    static __thread uint8_t wbuf[4096], rbuf[4096];
    const uint64_t base = MT_OFF + (uint64_t)a->idx * MT_SPAN;
    uint32_t k = 0;
    while (!g_race_stop_work && !g_stop && !a->ng) {
        bfm_conn_t *c = bfm_conn(a->port, a->i);
        if (c == NULL) { race_ng(a, "データの接続が使えなくなった", bfm_conn_state(a->port, a->i)); break; }
        int rc;
        for (uint32_t j = 0; j < sizeof(wbuf); j++) wbuf[j] = (uint8_t)(a->idx * 29u + k * 11u + j);
        if ((rc = bfm_write(c, base, wbuf, sizeof(wbuf))) != BFM_OK) { race_ng(a, "write 4KiB", rc); break; }
        memset(rbuf, 0, sizeof(rbuf));
        if ((rc = bfm_read(c, base, rbuf, sizeof(rbuf))) != BFM_OK) { race_ng(a, "read 4KiB", rc); break; }
        if (memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) { race_ng(a, "4KiB を読み戻すと違う", 0); break; }
        /* 多重度つき: 8 バイトの write を 16 個出して最後を待ち、128 バイトを読み戻す */
        uint64_t v[16], r[16];
        bfm_tag_t tag = 0;
        for (uint32_t j = 0; j < 16u && rc == BFM_OK; j++) {
            v[j] = ((uint64_t)a->idx << 48) | ((uint64_t)k << 8) | j;
            rc = bfm_write_async(c, base + 4096u + j * 8u, &v[j], 8, BFM_ACK, &tag);
        }
        if (rc == BFM_OK) rc = bfm_wait(c, tag);
        if (rc != BFM_OK) { race_ng(a, "多重度つきの write", rc); break; }
        if ((rc = bfm_read(c, base + 4096u, r, sizeof(r))) != BFM_OK) { race_ng(a, "read 128B", rc); break; }
        if (memcmp(v, r, sizeof(v)) != 0) { race_ng(a, "多重度つきの write を読み戻すと違う", 0); break; }
        a->ops++;
        k++;
    }
}

/* 張り直しの接続。張り直しを回すのは IP の小さいほう(掛け側)だけ: 使えるようになったら 8 バイトの write / read で
 * 確かめてから bfm_reconnect()。相手(受け側)は、BYE を受けて切れたら bfm_reconnect() を頼むだけで転送しない
 * (両側で転送すると、相手が消した QP 宛ての write が NIC の再送の尽きるまで約 4 秒戻らず、張り直しが止まる)。
 * cycles は「使えるようになった」回数(張り直しが 1 周した数)。 */
static void race_churn(struct race_conn *a)
{
    const uint64_t off = MT_OFF + (uint64_t)a->idx * MT_SPAN;
    const int leader = g_bfm_local_ip[a->port] < g_bfm_peer_ip[a->port];
    uint32_t seed = 0x9e3779b9u * (a->idx + 1u) ^ (uint32_t)now_ns();
    uint64_t k = 0;
    int was_up = 1;                                     /* 段 1 で張ってあるので、最初の UP は数えない */
    while (!g_race_stop_work && !g_stop && !a->ng) {
        const int st = bfm_conn_state(a->port, a->i);
        seed = seed * 1103515245u + 12345u;
        if (st == BFM_ST_FAILED) { race_ng(a, "張り直しの接続が失敗した(FAILED)", 0); break; }
        if (st == BFM_ST_UP && !was_up) a->cycles++;
        was_up = (st == BFM_ST_UP);
        if (st == BFM_ST_UP && leader) {
            usleep((seed >> 16) % 1000u);              /* 使える間を少し保つ(長さはばらつかせる)*/
            bfm_conn_t *c = bfm_conn(a->port, a->i);
            if (c != NULL) {
                const uint64_t v = ((uint64_t)a->idx << 48) | ++k;
                uint64_t r = 0;
                int rc = bfm_write(c, off, &v, 8);
                if (rc == BFM_OK) rc = bfm_read(c, off, &r, 8);
                if (rc != BFM_OK) a->errs++;
                else if (r != v) { race_ng(a, "write / read が通ったのに読み戻すと違う", 0); break; }
                else a->ops++;
            }
            (void)bfm_reconnect(a->port, a->i);
        } else if (st == BFM_ST_DOWN) {
            (void)bfm_reconnect(a->port, a->i);
            usleep(50);
        } else {
            usleep(50);                                 /* 確立の途中、または受け側で使える */
        }
    }
}

static void *race_conn_fn(void *p)
{
    struct race_conn *a = (struct race_conn *)p;
    if (a->churn) race_churn(a);
    else race_data(a);
    return NULL;
}

/* 見張り: 状態を読み続け、ときどき状態遷移の記録を出す(鍵を取る側の関数も同時に動かす)。 */
static void *race_watch_fn(void *p)
{
    const struct race_conn *churn0 = (const struct race_conn *)p;
    uint64_t next = now_ns() + 2000000000ull;
    while (!g_race_stop_work && !g_stop) {
        for (unsigned q = 0; q < BFM_MAX_PORT; q++)
            for (unsigned i = 0; i < g_bfm_port_conn[q]; i++) {
                (void)bfm_conn_state(q, i);
                g_race_watch_reads++;
            }
        if (churn0 != NULL && now_ns() >= next) {
            (void)bfm_conn_steplog(churn0->port, churn0->i);
            next = now_ns() + 2000000000ull;
        }
    }
    return NULL;
}

static void race_start_runners(struct race_runner *r, uint32_t n)
{
    g_race_stop_runner = 0;
    for (uint32_t t = 0; t < n; t++) {
        memset(&r[t], 0, sizeof(r[t]));
        pthread_create(&r[t].th, NULL, race_runner_fn, &r[t]);
    }
}

/* 回すスレッドを止め、戻り値の数を足して表示する。戻り値: 失敗を返した回数。 */
static uint64_t race_stop_runners(struct race_runner *r, uint32_t n, const char *what)
{
    g_race_stop_runner = 1;
    uint64_t calls = 0, ok = 0, inprog = 0, estate = 0, fail = 0;
    int first = 0;
    for (uint32_t t = 0; t < n; t++) {
        pthread_join(r[t].th, NULL);
        calls += r[t].calls; ok += r[t].ok; inprog += r[t].inprog; estate += r[t].estate;
        if (r[t].fail && !fail) first = r[t].first_fail;
        fail += r[t].fail;
    }
    printf("race: [%s] bfm_run() %u 本で %llu 回: OK %llu / INPROGRESS %llu / E_STATE %llu / 失敗 %llu%s%s\n", what, n,
           (unsigned long long)calls, (unsigned long long)ok, (unsigned long long)inprog, (unsigned long long)estate,
           (unsigned long long)fail, fail ? " 最初の失敗 " : "", fail ? bfm_strerror(first) : "");
    return fail;
}

/* 全部の接続が使えるようになるまで待つ(回すスレッドは別に動いている)。戻り値: 1 = そろった。 */
static int race_wait_all_up(uint64_t limit_ns)
{
    const uint64_t t0 = now_ns();
    while (now_ns() - t0 < limit_ns && !g_stop) {
        unsigned up = 0, total = 0;
        for (unsigned p = 0; p < BFM_MAX_PORT; p++)
            for (unsigned i = 0; i < g_bfm_port_conn[p]; i++) {
                total++;
                if (bfm_conn_state(p, i) == BFM_ST_UP) up++;
            }
        if (up == total) return 1;
        reconnect_down();
        usleep(1000);
    }
    return 0;
}

/* 全部の接続で 8 バイトの write / read を確かめる。戻り値: 通らなかった接続の数。 */
static unsigned race_verify_all(uint64_t salt)
{
    unsigned bad = 0;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++)
        for (unsigned i = 0; i < g_bfm_port_conn[p]; i++) {
            bfm_conn_t *c = bfm_conn(p, i);
            const uint64_t v = salt ^ ((uint64_t)(p * BFM_MAX_PORT_CONN + i) << 40);
            uint64_t r = 0;
            if (c == NULL || bfm_write(c, MT_OFF + (uint64_t)(p * BFM_MAX_PORT_CONN + i) * MT_SPAN + 8192u, &v, 8) != BFM_OK ||
                bfm_read(c, MT_OFF + (uint64_t)(p * BFM_MAX_PORT_CONN + i) * MT_SPAN + 8192u, &r, 8) != BFM_OK || r != v) {
                printf("race: ポート %u の接続 %u で write / read が通らない\n", p, i);
                bad++;
            }
        }
    return bad;
}

static int run_race(uint32_t nthreads, uint32_t secs)
{
    static struct race_runner runners[RACE_RUNNERS_MAX];
    static struct race_conn conns[BFM_MAX_CONN];
    if (nthreads == 0) nthreads = 1;
    if (nthreads > RACE_RUNNERS_MAX) nthreads = RACE_RUNNERS_MAX;
    if (secs == 0) secs = 30;
    g_ng = 0;

    /* 段 1 */
    if (run_up(nthreads) != BFM_OK) return 1;
    if (sync_at(400, "race の始め") != BFM_OK) return 1;

    /* 段 2 */
    unsigned nconn = 0, ndata = 0, nchurn = 0;
    const struct race_conn *churn0 = NULL;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++)
        for (unsigned i = 0; i < g_bfm_port_conn[p]; i++) {
            struct race_conn *a = &conns[nconn++];
            memset(a, 0, sizeof(*a));
            a->port = p;
            a->i = i;
            a->idx = p * BFM_MAX_PORT_CONN + i;
            a->churn = (i >= g_bfm_port_conn[p] / 2u);
            if (a->churn) { nchurn++; if (churn0 == NULL) churn0 = a; }
            else ndata++;
        }
    printf("race: 段 2: %u 秒。bfm_run() を休まず呼ぶスレッド %u 本、データの接続 %u 本、張り直しの接続 %u 本、見張り 1 本"
           "(CPU %ld 個)\n", secs, nthreads, ndata, nchurn, sysconf(_SC_NPROCESSORS_ONLN));
    g_race_stop_work = 0;
    g_race_reconnect = 0;
    g_race_watch_reads = 0;
    race_start_runners(runners, nthreads);
    for (unsigned t = 0; t < nconn; t++) pthread_create(&conns[t].th, NULL, race_conn_fn, &conns[t]);
    pthread_t watch;
    pthread_create(&watch, NULL, race_watch_fn, (void *)churn0);
    const uint64_t t_end = now_ns() + (uint64_t)secs * 1000000000ull;
    while (now_ns() < t_end && !g_stop) usleep(100000);
    g_race_stop_work = 1;
    for (unsigned t = 0; t < nconn; t++) pthread_join(conns[t].th, NULL);
    pthread_join(watch, NULL);

    uint64_t data_ops = 0, churn_ops = 0, churn_errs = 0, cycles = 0, cyc_min = ~0ull;
    unsigned ng_threads = 0;
    for (unsigned t = 0; t < nconn; t++) {
        const struct race_conn *a = &conns[t];
        if (a->churn) {
            churn_ops += a->ops; churn_errs += a->errs; cycles += a->cycles;
            if (a->cycles < cyc_min) cyc_min = a->cycles;
        } else data_ops += a->ops;
        if (a->ng) {
            ng_threads++;
            printf("race: ポート %u の接続 %u(%s): NG %s\n", a->port, a->i, a->churn ? "張り直し" : "データ", a->why);
        }
    }
    printf("race: データの接続: 確かめた回数 %llu(1 回 = 4KiB の write / read + 多重度つきの write 16 個 + read)\n",
           (unsigned long long)data_ops);
    printf("race: 張り直しの接続: 張り直して使えるようになった %llu 回(1 本あたり最少 %llu 回)、確かめて通った %llu 回、"
           "失敗した操作 %llu 回(張り直しを回すのは IP の小さいほう)\n", (unsigned long long)cycles,
           (unsigned long long)cyc_min, (unsigned long long)churn_ops, (unsigned long long)churn_errs);
    printf("race: 見張り: 状態を %llu 回読んだ\n", (unsigned long long)g_race_watch_reads);
    check("race: [2] データの接続の転送が一度も失敗しない・読み戻しが一度も違わない", ng_threads == 0);
    check("race: [2] 張り直しの接続が、どれも 1 回以上張り直された", nchurn == 0 || cyc_min > 0);

    /* 段 3 */
    const int all_up = race_wait_all_up(60ull * 1000000000ull);
    check("race: [3] 張り直しをやめると、全部の接続が使えるようになる", all_up);
    const uint64_t fail2 = race_stop_runners(runners, nthreads, "段 2・3");
    check("race: [2][3] bfm_run() が失敗した接続を一度も返さない", fail2 == 0);
    check("race: [3] 全部の接続で 8 バイトの write / read が通る", all_up && race_verify_all(0x1111000000000000ull) == 0);
    if (sync_at(401, "race の止める前") != BFM_OK) return 1;

    /* 段 4: 回すスレッドを動かしたまま止め、立ち上げ直させる */
    g_race_reconnect = 1;
    race_start_runners(runners, nthreads);
    usleep(20000);
    const uint64_t t0 = now_ns();
    do_shutdown();
    const uint64_t t_stop = now_ns() - t0;
    printf("race: 段 4: bfm_run() を %u 本で回したまま bfm_shutdown() が %.1f ms で済んだ\n", nthreads, (double)t_stop / 1e6);
    check("race: [4] bfm_run() を回したまま bfm_shutdown() が済む", 1);
    const int up4 = race_wait_all_up(120ull * 1000000000ull);
    printf("race: 段 4: 止めてから %.2f 秒で、立ち上げ直して全部の接続が使える%s\n", (double)(now_ns() - t0) / 1e9,
           up4 ? "" : "(そろわない)");
    check("race: [4] 回しているスレッドが立ち上げ直し、全部の接続が使えるようになる", up4);
    const uint64_t fail4 = race_stop_runners(runners, nthreads, "段 4");
    check("race: [4] bfm_run() が失敗を一度も返さない(立ち上げ直しの取り合いを含む)", fail4 == 0);
    /* 立ち上げ直しで公開メモリは張り直され 0 になっている。埋め直さない(相手が先に書いた足並みの語を消さないため)*/
    if (sync_at(402, "race の立ち上げ直し") != BFM_OK) return 1;
    check("race: [4] 全部の接続で 8 バイトの write / read が通る", up4 && race_verify_all(0x4444000000000000ull) == 0);
    if (sync_at(403, "race の終わり") != BFM_OK) return 1;
    printf("race: 結果 %s(NG %d 件)\n", g_ng ? "NG" : "OK", g_ng);
    return g_ng ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * wpost: 全部のコアで同時に、8 バイトの bfm_write_post() の速さを測る(両方のノードで wpost を動かす = 両方向)
 *
 * そのノードの CPU に 1 本ずつスレッドを固定し、コア k は接続 (ポート k % ポートの数, k / ポートの数) を 1 本だけ使う。
 * 各スレッドは相手の公開メモリの自分の区画へ、8 バイトの bfm_write_post() を -n 秒(既定 5)打ち続け、最後に
 * bfm_flush() で全部の完了を待つ。これを WPOST_ROUNDS 回(回ごとに相手と足並みをそろえる)。コアごとの速さと、
 * コアの間の偏り(最大 / 最小・変動係数)を出す。測る間は bfm_run() を回すスレッドを置かない(全部のコアを測る側に使う)。
 * ------------------------------------------------------------------- */

#define WPOST_ROUNDS 3u
#define WPOST_CPU_MAX 256u

struct wpost_arg {
    pthread_t th;
    unsigned cpu, port, i;               /* 固定するコアと、使う接続 */
    bfm_conn_t *c;
    uint64_t ops;                        /* 打った回数 */
    uint64_t ns;                         /* 打ち始めてから bfm_flush() が済むまで */
    int rc;                              /* 最初の誤り(BFM_OK = なし)*/
    int cpu_ok;                          /* 1 = そのコアに固定できた */
};

static pthread_barrier_t g_wp_bar;
static uint64_t g_wp_ns;                 /* 1 回の長さ(ns)*/

static void *wpost_worker(void *p)
{
    struct wpost_arg *a = (struct wpost_arg *)p;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET((int)a->cpu, &cs);
    a->cpu_ok = (pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) == 0);
    const uint64_t base = MT_OFF + (uint64_t)(a->port * BFM_MAX_PORT_CONN + a->i) * MT_SPAN;
    uint64_t v = ((uint64_t)a->cpu << 48), k = 0;
    a->rc = BFM_OK;
    pthread_barrier_wait(&g_wp_bar);                      /* 全部のスレッドで同時に始める */
    const uint64_t t0 = now_ns(), end = t0 + g_wp_ns;
    for (;;) {
        for (unsigned j = 0; j < 256u; j++, k++) {          /* 時刻は 256 回ごとに見る */
            v = ((uint64_t)a->cpu << 48) | k;
            const int rc = bfm_write_post(a->c, base + (k & 63u) * 8u, &v, 8);
            if (rc != BFM_OK) { a->rc = rc; goto out; }
        }
        if (now_ns() >= end) break;
    }
out:;
    const int rc = bfm_flush(a->c);
    if (a->rc == BFM_OK && rc != BFM_OK) a->rc = rc;
    a->ns = now_ns() - t0;
    a->ops = k;
    /* 最後に書いた値が相手のメモリに届いているか(k - 1 回目の値は base + ((k - 1) & 63) * 8)*/
    if (a->rc == BFM_OK && k > 0) {
        uint64_t r = 0;
        const int rr = bfm_read(a->c, base + ((k - 1u) & 63u) * 8u, &r, 8);
        if (rr != BFM_OK) a->rc = rr;
        else if (r != v) a->rc = BFM_E_HW;
    }
    pthread_barrier_wait(&g_wp_bar);
    return NULL;
}

/* 平方根(ニュートン法。libm を使わないため)。 */
static double wp_sqrt(double x)
{
    if (x <= 0) return 0;
    double y = x > 1 ? x : 1;
    for (int i = 0; i < 60; i++) y = 0.5 * (y + x / y);
    return y;
}

static int run_wpost(uint32_t secs)
{
    static struct wpost_arg args[WPOST_CPU_MAX];
    static double mops[WPOST_ROUNDS][WPOST_CPU_MAX];
    if (secs == 0) secs = 5;
    g_wp_ns = (uint64_t)secs * 1000000000ull;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    if (ncpu > (long)WPOST_CPU_MAX) ncpu = WPOST_CPU_MAX;
    unsigned ports[BFM_MAX_PORT], np = 0;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++)
        if (g_bfm_port_conn[p] != 0) ports[np++] = p;
    g_ng = 0;
    if (run_up(4) != BFM_OK) return 1;

    /* コア k -> j = (k + -R) % CPU の数 番目の接続 = (ポート j % np, j / np) */
    for (unsigned k = 0; k < (unsigned)ncpu; k++) {
        struct wpost_arg *a = &args[k];
        const unsigned j = (k + g_wp_rot) % (unsigned)ncpu;
        memset(a, 0, sizeof(*a));
        a->cpu = k;
        a->port = ports[j % np];
        a->i = j / np;
        if (a->i >= g_bfm_port_conn[a->port] || (a->c = bfm_conn(a->port, a->i)) == NULL) {
            printf("wpost: コア %u に使う接続 (ポート %u, %u) が無い。ポートの接続の数(-c)を増やすこと\n", k, a->port, a->i);
            return 1;
        }
    }
    printf("wpost: CPU %ld 個に 1 本ずつスレッドを固定し、コアごとに接続 1 本で 8 バイトの bfm_write_post() を %u 秒 x %u 回"
           "(相手も同時に打つ = 両方向。ポート %u 個に交互に割り当て。inline %u バイトまで、BlueFlame %s)\n",
           ncpu, secs, WPOST_ROUNDS, np, bfm_inline_max(), ((struct bfm_conn *)args[0].c)->bf ? "あり" : "なし");

    for (unsigned r = 0; r < WPOST_ROUNDS && !g_stop; r++) {
        snprintf(g_mt_msg, sizeof(g_mt_msg), "wpost の %u 回目", r + 1u);
        if (sync_at(500u + r, g_mt_msg) != BFM_OK) return 1;
        pthread_barrier_init(&g_wp_bar, NULL, (unsigned)ncpu);
        for (unsigned k = 0; k < (unsigned)ncpu; k++) pthread_create(&args[k].th, NULL, wpost_worker, &args[k]);
        for (unsigned k = 0; k < (unsigned)ncpu; k++) pthread_join(args[k].th, NULL);
        pthread_barrier_destroy(&g_wp_bar);

        double sum = 0, mn = 1e30, mx = 0, sq = 0;
        unsigned bad = 0, kmin = 0, kmax = 0;
        printf("wpost: [%u 回目] コア ポート 接続       回数     Mops  1 回 ns\n", r + 1u);
        for (unsigned k = 0; k < (unsigned)ncpu; k++) {
            const struct wpost_arg *a = &args[k];
            const double m = (double)a->ops / ((double)a->ns / 1e9) / 1e6;
            mops[r][k] = m;
            sum += m;
            sq += m * m;
            if (m < mn) { mn = m; kmin = k; }
            if (m > mx) { mx = m; kmax = k; }
            if (a->rc != BFM_OK || !a->cpu_ok) bad++;
            printf("wpost:            %4u %6u %4u %10llu %8.3f %8.1f%s%s\n", a->cpu, a->port, a->i,
                   (unsigned long long)a->ops, m, (double)a->ns / (double)a->ops, a->cpu_ok ? "" : "  コアに固定できない",
                   a->rc != BFM_OK ? "  誤り" : "");
            if (a->rc != BFM_OK) printf("wpost:   コア %u: %s\n", a->cpu, bfm_strerror(a->rc));
        }
        const double avg = sum / (double)ncpu;
        const double sd = wp_sqrt(sq / (double)ncpu - avg * avg);
        printf("wpost: [%u 回目] 合計 %.2f Mops(%.1f MB/s)、1 コアあたり 平均 %.3f / 最小 %.3f(コア %u)/ 最大 %.3f(コア %u)Mops、"
               "最大/最小 %.3f、変動係数 %.1f%%\n", r + 1u, sum, sum * 8.0, avg, mn, kmin, mx, kmax,
               mn > 0 ? mx / mn : 0.0, avg > 0 ? sd / avg * 100.0 : 0.0);
        snprintf(g_mt_msg, sizeof(g_mt_msg), "wpost: [%u 回目] 全部のコアで誤りなし(最後に書いた値が届いている)", r + 1u);
        check(g_mt_msg, bad == 0);
    }
    if (sync_at(500u + WPOST_ROUNDS, "wpost の終わり") != BFM_OK) return 1;

    /* 3 回の平均(コアごと)*/
    printf("wpost: [%u 回の平均] コア ポート  Mops(回ごと)\n", WPOST_ROUNDS);
    double sum = 0, mn = 1e30, mx = 0;
    for (unsigned k = 0; k < (unsigned)ncpu; k++) {
        double m = 0;
        for (unsigned r = 0; r < WPOST_ROUNDS; r++) m += mops[r][k];
        m /= WPOST_ROUNDS;
        sum += m;
        if (m < mn) mn = m;
        if (m > mx) mx = m;
        printf("wpost:              %4u %6u  %.3f(%.3f / %.3f / %.3f)\n", k, args[k].port, m, mops[0][k], mops[1][k], mops[2][k]);
    }
    printf("wpost: [%u 回の平均] 合計 %.2f Mops、1 コアあたり 平均 %.3f / 最小 %.3f / 最大 %.3f Mops、最大/最小 %.3f\n",
           WPOST_ROUNDS, sum, sum / (double)ncpu, mn, mx, mn > 0 ? mx / mn : 0.0);
    printf("wpost: 結果 %s(NG %d 件)\n", g_ng ? "NG" : "OK", g_ng);
    return g_ng ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * perf: 物理コアの数 1〜4 で、操作と長さごとの IOPS・MB/s・平均の遅延を測る(両方のノードで perf を動かす = 両方向)
 *
 * N 本(N = 1〜PERF_CORES)のスレッドを CPU 0〜N-1 に固定し(Precision の HT の相方は CPU k+8 なので、どれも別の
 * 物理コア)、スレッド k は接続 (ポート k % ポートの数, k / ポートの数) で、相手の公開メモリの自分の区画
 * (PERF_OFF + k * PERF_SPAN)へ -n 秒(既定 2)打ち続ける。項目ごとに相手と足並みをそろえ、相手も同時に同じ項目を打つ。
 * 平均の遅延: 1 回ずつ待つ操作は「かかった時間 / 回数」。bfm_read_async() は g_bfm_qdepth 個出しておき、1 個ずつ
 * 出してから完了を見るまで。bfm_write_post() は完了を 1 個ずつ見られないので、1 回出すのにかかる時間。
 * 結果は「perf|」で始まる 1 行ずつ出す(仕様書の 20 章の表とグラフは、この行から作る)。
 * ------------------------------------------------------------------- */

#define PERF_CORES  4u
#define PERF_OFF    (64u * 1024u * 1024u)
#define PERF_SPAN   (1024u * 1024u)
#define PERF_BIG    32768u
#define PERF_DIF_MAX 512u

enum { PF_WPOST = 0, PF_READ, PF_RASYNC, PF_CAS, PF_FAA, PF_DIFW, PF_DIFR };
static const char *const g_pf_name[] = {"bfm_write_post", "bfm_read", "bfm_read_async", "bfm_cas", "bfm_faa",
                                        "bfm_dif_write", "bfm_dif_read"};

struct perf_arg {
    pthread_t th;
    unsigned cpu;
    bfm_conn_t *c;
    int kind;
    uint32_t size;                       /* 長さ(バイト。DIF はブロックの数)*/
    uint64_t ops, ns, lat_ns;            /* 回数・かかった時間・遅延の合計(read_async だけ)*/
    uint64_t lat_max, n1ms, n10ms, n100ms;   /* read_async の遅延の最大と、1ms・10ms・100ms を超えた数 */
    int rc;
};

static const char *g_pf_only;            /* -T: perf で、名前にこの文字列を含む項目だけを測る(NULL = 全部)*/

static pthread_barrier_t g_pf_bar;
static uint64_t g_pf_ns;
/* スレッド(CPU)ごとの read_async の着地点と DIF の元 / 着地点。一度だけ取り、解放しない(read_async の応答は、
 * 完了を刈り取ったときに写される。誤りで止めたときに仕掛かり中のものが残っていると、後で別の関数(足並みを
 * そろえる bfm_write() など)が刈り取って写すので、着地点を手放してはいけない)。 */
static uint8_t *g_pf_abuf[PERF_CORES], *g_pf_dbuf[PERF_CORES];

/* n ブロックの 520 バイトおきのデータを、正しい PI つきで作る(app tag 0x1234、ref tag 0x1000 から)。 */
static void perf_dif_make(uint8_t *out, uint32_t n, unsigned seed)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *b = out + i * (BFM_DIF_BLOCK + BFM_DIF_PI);
        for (uint32_t j = 0; j < BFM_DIF_BLOCK; j++) b[j] = (uint8_t)(seed * 31u + i * 7u + j);
        const uint16_t g = crc16_bitwise(b, BFM_DIF_BLOCK);
        const uint32_t r = 0x1000u + i;
        b[512] = (uint8_t)(g >> 8); b[513] = (uint8_t)g;
        b[514] = 0x12; b[515] = 0x34;
        b[516] = (uint8_t)(r >> 24); b[517] = (uint8_t)(r >> 16); b[518] = (uint8_t)(r >> 8); b[519] = (uint8_t)r;
    }
}

static void *perf_worker(void *p)
{
    struct perf_arg *a = (struct perf_arg *)p;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET((int)a->cpu, &cs);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    const uint64_t base = PERF_OFF + (uint64_t)a->cpu * PERF_SPAN;
    const uint32_t qd = ((struct bfm_conn *)a->c)->aqd;
    static __thread uint8_t src[PERF_BIG], dst[PERF_BIG];
    if (g_pf_abuf[a->cpu] == NULL) g_pf_abuf[a->cpu] = malloc((size_t)BFM_QD_MAX * PERF_BIG);
    if (g_pf_dbuf[a->cpu] == NULL) g_pf_dbuf[a->cpu] = malloc((size_t)PERF_DIF_MAX * (BFM_DIF_BLOCK + BFM_DIF_PI));
    uint8_t *abuf = g_pf_abuf[a->cpu];   /* read_async の着地点(qd 個)*/
    uint8_t *dbuf = g_pf_dbuf[a->cpu];   /* DIF の 520 バイトおきの元 / 着地点 */
    bfm_dif_t d;
    bfm_dif_err_t e;
    memset(&d, 0, sizeof(d));
    d.op = BFM_DIF_CHECK;
    d.mode = BFM_DIF_SW;
    d.check = BFM_DIF_CHK_ALL;
    d.app_tag = 0x1234;
    d.ref_tag = 0x1000;
    for (uint32_t j = 0; j < sizeof(src); j++) src[j] = (uint8_t)(a->cpu + j);
    a->rc = BFM_OK;
    if (abuf == NULL || dbuf == NULL) a->rc = BFM_E_NOMEM;
    else if (a->kind == PF_DIFW || a->kind == PF_DIFR) {
        {
            perf_dif_make(dbuf, a->size, a->cpu);
            /* read のために、相手のメモリへ正しい PI つきのデータを置いておく */
            if (a->kind == PF_DIFR) a->rc = bfm_dif_write(a->c, base, dbuf, a->size, &d, &e);
        }
    }
    pthread_barrier_wait(&g_pf_bar);                      /* 全部のスレッドで同時に始める */
    const uint64_t t0 = now_ns(), end = t0 + g_pf_ns;
    uint64_t k = 0, lat = 0;
    int rc = a->rc;
    if (rc == BFM_OK) {
        switch (a->kind) {
        case PF_WPOST:
            for (;;) {
                for (unsigned j = 0; j < 256u && rc == BFM_OK; j++, k++)
                    rc = bfm_write_post(a->c, base + (k & 63u) * 64u, src, a->size);
                if (rc != BFM_OK || now_ns() >= end) break;
            }
            if (rc == BFM_OK) rc = bfm_flush(a->c);
            break;
        case PF_READ:
            while (rc == BFM_OK && now_ns() < end) {
                rc = bfm_read(a->c, base, dst, a->size);
                k++;
            }
            break;
        case PF_RASYNC: {
            /* qd 個出しておき、古い順に待つ(出してから完了を見るまでを遅延として足す)*/
            bfm_tag_t tag[BFM_QD_MAX];
            uint64_t tp[BFM_QD_MAX];
            uint32_t head = 0, n = 0;
            int stop = 0;
            while (rc == BFM_OK && (n > 0 || !stop)) {
                if (!stop && n < qd) {
                    const uint32_t s = (head + n) % qd;
                    tp[s] = now_ns();
                    rc = bfm_read_async(a->c, base, abuf + (size_t)s * a->size, a->size, &tag[s]);
                    n++;
                    if (tp[s] >= end) stop = 1;
                    continue;
                }
                rc = bfm_wait(a->c, tag[head]);
                const uint64_t dl = now_ns() - tp[head];
                lat += dl;
                if (dl > a->lat_max) a->lat_max = dl;
                a->n1ms += (dl > 1000000u);
                a->n10ms += (dl > 10000000u);
                a->n100ms += (dl > 100000000u);
                k++;
                head = (head + 1u) % qd;
                n--;
            }
            break;
        }
        case PF_CAS:
            while (rc == BFM_OK && now_ns() < end) {
                if (a->size == 4) { uint32_t o; rc = bfm_cas32(a->c, base + 8192u, (uint32_t)k, (uint32_t)k + 1u, &o); }
                else { uint64_t o; rc = bfm_cas(a->c, base + 8192u, k, k + 1u, &o); }
                k++;
            }
            break;
        case PF_FAA:
            while (rc == BFM_OK && now_ns() < end) {
                if (a->size == 4) { uint32_t o; rc = bfm_faa32(a->c, base + 8192u, 1, &o); }
                else { uint64_t o; rc = bfm_faa(a->c, base + 8192u, 1, &o); }
                k++;
            }
            break;
        case PF_DIFW:
            while (rc == BFM_OK && now_ns() < end) {
                rc = bfm_dif_write(a->c, base, dbuf, a->size, &d, &e);
                k++;
            }
            break;
        default:   /* PF_DIFR */
            while (rc == BFM_OK && now_ns() < end) {
                rc = bfm_dif_read(a->c, base, dbuf, a->size, &d, &e);
                k++;
            }
            break;
        }
    }
    a->ns = now_ns() - t0;
    a->ops = k;
    a->lat_ns = lat;
    a->rc = rc;
    pthread_barrier_wait(&g_pf_bar);
    return NULL;
}

static int run_perf(uint32_t secs)
{
    static struct perf_arg args[PERF_CORES];
    static const struct { int kind; uint32_t size; } items[] = {
        {PF_WPOST, 4}, {PF_WPOST, 64},   /* bfm_write_post() は bfm_inline_max()(220 バイト)までなので 256 バイト以上は測らない */
        {PF_READ, 4}, {PF_READ, 64}, {PF_READ, 256}, {PF_READ, PERF_BIG},
        {PF_RASYNC, 4}, {PF_RASYNC, 64}, {PF_RASYNC, 256}, {PF_RASYNC, PERF_BIG},
        {PF_CAS, 4}, {PF_CAS, 8}, {PF_FAA, 4}, {PF_FAA, 8},
        {PF_DIFW, 16}, {PF_DIFW, 64}, {PF_DIFW, PERF_DIF_MAX},
        {PF_DIFR, 16}, {PF_DIFR, 64}, {PF_DIFR, PERF_DIF_MAX},
    };
    if (secs == 0) secs = 2;
    g_pf_ns = (uint64_t)secs * 1000000000ull;
    unsigned ports[BFM_MAX_PORT], np = 0;
    for (unsigned p = 0; p < BFM_MAX_PORT; p++)
        if (g_bfm_port_conn[p] != 0) ports[np++] = p;
    g_ng = 0;
    if (run_up(4) != BFM_OK) return 1;
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < (long)PERF_CORES) {
        printf("perf: CPU が %ld 個しかない(%u 個要る)\n", ncpu, PERF_CORES);
        return 1;
    }
    for (unsigned k = 0; k < PERF_CORES; k++) {
        const unsigned port = ports[k % np], i = k / np;
        if ((args[k].c = bfm_conn(port, i)) == NULL) {
            printf("perf: スレッド %u に使う接続 (ポート %u, %u) が使えない\n", k, port, i);
            return 1;
        }
    }
    printf("perf: スレッドを CPU 0〜N-1 に固定し(N = 1〜%u)、スレッドごとに接続 1 本(ポート %u 個に交互)で %u 秒ずつ。"
           "相手も同時に同じ項目を打つ(両方向)。inline %u バイトまで、read_async の多重度 %u・1 回の上限 %u バイト、"
           "DIF は SW の CHECK\n", PERF_CORES, np, secs, bfm_inline_max(), ((struct bfm_conn *)args[0].c)->aqd,
           g_bfm_async_buf_size);
    printf("perf|項目|長さ|コア|IOPS|MB/s|平均の遅延 ns|誤り\n");
    uint64_t stage = 600;
    for (unsigned it = 0; it < sizeof(items) / sizeof(items[0]) && !g_stop; it++) {
        const int kind = items[it].kind;
        uint32_t size = items[it].size;
        if (kind == PF_WPOST && size > bfm_inline_max()) {
            printf("perf|%s|%u|-|対象外(bfm_write_post() は bfm_inline_max() = %u バイトまで)\n", g_pf_name[kind], size,
                   bfm_inline_max());
            continue;
        }
        /* -T 項目名[:長さ] で絞る(両方のノードで同じに)。長さを書けば、その項目の最初の行だけを、その長さで測る */
        uint32_t size2 = size;
        if (g_pf_only != NULL) {
            char want[64];
            const char *colon = strchr(g_pf_only, ':');
            const size_t wl = colon ? (size_t)(colon - g_pf_only) : strlen(g_pf_only);
            snprintf(want, sizeof(want), "%.*s", (int)(wl < sizeof(want) ? wl : sizeof(want) - 1u), g_pf_only);
            if (strstr(g_pf_name[kind], want) == NULL) continue;
            if (colon != NULL) {
                if (it > 0 && items[it - 1].kind == kind) continue;
                size2 = (uint32_t)atoi(colon + 1);
            }
        }
        size = size2;
        const uint32_t bytes = (kind == PF_DIFW || kind == PF_DIFR) ? size * BFM_DIF_BLOCK : size;
        for (unsigned n = 1; n <= PERF_CORES && !g_stop; n++) {
            /* 前の測定の誤りで切れた接続は張り直してから測る(使う接続が全部使えるまで。30 秒まで)*/
            const uint64_t tw = now_ns();
            for (;;) {
                unsigned up = 0;
                for (unsigned k = 0; k < PERF_CORES; k++) up += (bfm_conn(ports[k % np], k / np) != NULL);
                if (up == PERF_CORES || now_ns() - tw > 30000000000ull) break;
                (void)bfm_run();
                reconnect_down();
                usleep(1000);
            }
            snprintf(g_mt_msg, sizeof(g_mt_msg), "perf %s %u x%u", g_pf_name[kind], size, n);
            if (sync_at(stage++, g_mt_msg) != BFM_OK) return 1;
            if (g_quiet) {                                 /* -q: 打たない(相手だけが打つ = 片方向)*/
                printf("perf|%s|%u|%u|受けるだけ(-q)\n", g_pf_name[kind], size, n);
                continue;
            }
            pthread_barrier_init(&g_pf_bar, NULL, n);
            for (unsigned k = 0; k < n; k++) {
                memset(&args[k].ops, 0, sizeof(args[k]) - offsetof(struct perf_arg, ops));
                args[k].cpu = k;
                args[k].kind = kind;
                args[k].size = size;
                pthread_create(&args[k].th, NULL, perf_worker, &args[k]);
            }
            for (unsigned k = 0; k < n; k++) pthread_join(args[k].th, NULL);
            pthread_barrier_destroy(&g_pf_bar);
            double iops = 0, lat = 0;
            uint64_t ops = 0, lat_sum = 0;
            int bad = 0;
            for (unsigned k = 0; k < n; k++) {
                const struct perf_arg *a = &args[k];
                if (a->rc != BFM_OK) {
                    bad++;
                    printf("perf: %s %u コア %u: %s\n", g_pf_name[kind], size, k, bfm_strerror(a->rc));
                }
                if (a->ops && a->ns) iops += (double)a->ops / ((double)a->ns / 1e9);
                if (a->ops) lat += (double)a->ns / (double)a->ops;
                ops += a->ops;
                lat_sum += a->lat_ns;
            }
            lat = (kind == PF_RASYNC) ? (ops ? (double)lat_sum / (double)ops : 0) : lat / n;
            printf("perf|%s|%u|%u|%.0f|%.2f|%.0f|%d\n", g_pf_name[kind], size, n, iops, iops * bytes / 1e6, lat, bad);
            if (kind == PF_RASYNC) {                       /* スレッドごとの速さと遅延の偏り */
                for (unsigned k = 0; k < n; k++) {
                    const struct perf_arg *a = &args[k];
                    printf("perf-a|%u|%u|スレッド %u(ポート %u)|%.1f MB/s|平均 %.0f µs|最大 %.1f ms|>1ms %llu|>10ms %llu|>100ms %llu|回数 %llu\n",
                           size, n, k, ports[k % np], a->ns ? (double)a->ops * size / ((double)a->ns / 1e3) : 0.0,
                           a->ops ? (double)a->lat_ns / (double)a->ops / 1e3 : 0.0, (double)a->lat_max / 1e6,
                           (unsigned long long)a->n1ms, (unsigned long long)a->n10ms, (unsigned long long)a->n100ms,
                           (unsigned long long)a->ops);
                }
            }
            if (bad) g_ng++;
        }
    }
    if (sync_at(stage, "perf の終わり") != BFM_OK) return 1;
    printf("perf: 結果 %s(誤りのあった測定 %d 件)\n", g_ng ? "NG" : "OK", g_ng);
    return g_ng ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * crc: T10-DIF の CRC16 だけを確かめる(相手も NIC も要らない)。
 *   1. 使う方法(PCLMULQDQ があればそれ)・表だけ・1 ビットずつ の 3 つの値が、長さ 0〜2100 バイトと、乱数の長さ・
 *      位置(16 バイトの境界からずらす)で一致すること
 *   2. 512 バイト(1 ブロック)ずつの速さ(表だけ / 使う方法)。-n 秒(既定 1)ずつ
 * ------------------------------------------------------------------- */

static int run_crc(uint32_t secs)
{
    static uint8_t buf[1u << 20];
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (uint32_t i = 0; i < sizeof(buf); i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        buf[i] = (uint8_t)x;
    }
    printf("crc: CPU が PCLMULQDQ の方法を使う: %s(g_bfm_dif_clmul = %u)\n", bfm_dif_crc_clmul() ? "はい" : "いいえ",
           g_bfm_dif_clmul);
    /* 1. 一致 */
    uint32_t bad = 0, n = 0;
    for (uint32_t len = 0; len <= 2100u; len++, n++) {
        const uint8_t *p = buf + (len * 7u) % 64u;
        const uint16_t a = bfm_dif_crc16(p, len), b = bfm_dif_crc16_table(p, len), c = crc16_bitwise(p, len);
        if (a != b || b != c) {
            if (bad++ < 5) printf("crc: 長さ %u で違う(使う方法 0x%04x / 表 0x%04x / 1 ビットずつ 0x%04x)\n", len, a, b, c);
        }
    }
    for (uint32_t k = 0; k < 20000u; k++, n++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        const uint32_t len = (uint32_t)(x % 70000u), off = (uint32_t)((x >> 32) % 4096u);
        const uint16_t a = bfm_dif_crc16(buf + off, len), b = bfm_dif_crc16_table(buf + off, len);
        if (a != b) {
            if (bad++ < 5) printf("crc: 長さ %u・位置 %u で違う(使う方法 0x%04x / 表 0x%04x)\n", len, off, a, b);
        }
    }
    printf("crc: [1] %u 通りで、使う方法・表・(2100 バイトまでは)1 ビットずつが一致: %s(違い %u)\n", n, bad ? "NG" : "OK", bad);
    /* 2. 速さ(512 バイトずつ。1MiB の中を順に)*/
    if (secs == 0) secs = 1;
    for (int m = 0; m < 2; m++) {
        const uint64_t t0 = now_ns(), end = t0 + (uint64_t)secs * 1000000000ull;
        uint64_t blocks = 0;
        uint32_t acc = 0;
        while (now_ns() < end) {
            for (uint32_t off = 0; off < sizeof(buf); off += BFM_DIF_BLOCK, blocks++)
                acc ^= m ? bfm_dif_crc16(buf + off, BFM_DIF_BLOCK) : bfm_dif_crc16_table(buf + off, BFM_DIF_BLOCK);
        }
        const double ns = (double)(now_ns() - t0);
        printf("crc: [2] %-18s 512 バイトあたり %6.1f ns、%6.2f GB/s(%llu ブロック、検算 %04x)\n",
               m ? (bfm_dif_crc_clmul() ? "PCLMULQDQ" : "使う方法(表)") : "表(slice-by-8)", ns / (double)blocks,
               (double)blocks * BFM_DIF_BLOCK / ns, (unsigned long long)blocks, acc);
    }
    return bad ? 1 : 0;
}

/* info: ポートを立ち上げて能力を表示する(接続は待たない)。 */
static int run_info(void)
{
    const uint64_t t0 = now_ns();
    int pending = 1;
    while (pending && !g_stop && now_ns() - t0 < 180ull * 1000000000ull) {
        const int rc = bfm_run();
        if (rc < 0 && rc != BFM_E_STATE) return 1;
        pending = 0;
        for (unsigned p = 0; p < BFM_MAX_PORT; p++)
            if (g_bfm_port_conn[p] != 0 && bfm_conn_state(p, 0) == BFM_ST_INIT) pending = 1;
        idle();
    }
    for (unsigned p = 0; p < BFM_MAX_PORT; p++) {
        const bfm_dev_t *d = &g_bfm_dev[p];
        if (!d->used) continue;
        printf("info: ポート %u: %s bf=%u bf_buf=%u atomic ops=0x%x size_qp=0x%x mode=%u ts_format=%u "
               "read/atomic req/res=%u/%u FW ページ %u sho=%u\n", p, d->name ? d->name : "?",
               d->bf_cap, d->bf_buf_size, d->atomic_ops, d->atomic_size_qp, d->atomic_mode,
               d->qp_ts_format, 1u << d->log_max_ra_req_qp, 1u << d->log_max_ra_res_qp,
               d->fw_pages_used, d->sho);
    }
    printf("info: T10-DIF オフロード %s\n", bfm_dif_hw_supported() ? "あり" : "なし");
    return 0;
}

/* ---------------------------------------------------------------------
 * fw: FW の更新(bfm_fw_update())
 * ------------------------------------------------------------------- */

/* ポート 0 を立ち上げ、path の FW の像を bfm_fw_update() で書く。activate が 0 なら確かめるだけ
 * (FW に書かせて確かめさせ、切り替えずに取り消す)。Ctrl-C で取り消す。 */
static int run_fw(const char *path, int activate)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("fw: %s を開けない\n", path);
        return 1;
    }
    uint8_t *img = NULL;
    long len = 0;
    if (fseek(fp, 0, SEEK_END) == 0) len = ftell(fp);
    if (len > 0 && len < (64l << 20) && fseek(fp, 0, SEEK_SET) == 0) {
        img = (uint8_t *)malloc((size_t)len);
        if (img != NULL && fread(img, 1, (size_t)len, fp) != (size_t)len) {
            free(img);
            img = NULL;
        }
    }
    fclose(fp);
    if (img == NULL) {
        printf("fw: %s を読めない (長さ %ld)\n", path, len);
        return 1;
    }
    printf("fw: %s (%ld バイト) を%s\n", path, len, activate ? "書いて切り替える" : "書いて確かめるだけ(切り替えない)");

    /* ポート 0 の立ち上げを待つ(接続はできなくてもよい)*/
    const uint64_t t0 = now_ns();
    while (!bfm_dev_ready(&g_bfm_dev[0])) {
        const int rc = bfm_run();
        if ((rc < 0 && rc != BFM_E_STATE) || g_stop || now_ns() - t0 > 180ull * 1000000000ull) {
            printf("fw: ポート 0 が立ち上がらない\n");
            free(img);
            return 1;
        }
        idle();
    }
    const uint64_t t1 = now_ns();
    int rc, cancelled = 0;
    while ((rc = bfm_fw_update(0, img, (uint32_t)len, activate ? BFM_FW_ACTIVATE : BFM_FW_VERIFY_ONLY)) ==
           BFM_INPROGRESS) {
        if (g_stop && !cancelled) {
            bfm_fw_update_cancel(0);
            cancelled = 1;
        }
        (void)bfm_run();                 /* FW のページの要求などに答え続ける */
    }
    printf("fw: 結果 %d (%s)、%.1f 秒\n", rc, bfm_strerror(rc), (double)(now_ns() - t1) / 1e9);
    free(img);
    return rc == BFM_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    uint32_t iters = 0, threads = 4, nconn = 0;
    int oob = 0, fw_activate = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d:i:D:I:c:p:m:M:n:t:Q:a:R:T:BSCxqPzLvAh")) != -1) {
        switch (opt) {
        case 'd': g_bfm_linux_pci_bdf[0] = optarg; break;
        case 'i': if (parse_ip(optarg, &g_bfm_local_ip[0]) != 0) { usage(); return 2; } break;
        case 'D': g_bfm_linux_pci_bdf[1] = optarg; break;
        case 'I': if (parse_ip(optarg, &g_bfm_local_ip[1]) != 0) { usage(); return 2; } break;
        case 'c': nconn = (uint32_t)atoi(optarg); break;
        case 'p': g_bfm_cm_port = (uint16_t)atoi(optarg); break;
        case 'm': g_bfm_path_mtu = (uint8_t)atoi(optarg); break;
        case 'M': g_bfm_port_mtu = (uint16_t)atoi(optarg); break;
        case 'n': iters = (uint32_t)atoi(optarg); break;
        case 't': threads = (uint32_t)atoi(optarg); break;
        case 'B': g_bfm_bf_enable = 0; break;
        case 'S': g_bfm_scatter_cqe = 0; break;
        case 'C': g_bfm_cpu_awake = 0; break;
        case 'x': oob = 1; break;
        case 'q': g_quiet = 1; break;
        case 'P': g_perf = 1; break;
        case 'z': g_silent = 1; break;
        case 'R': g_wp_rot = (unsigned)atoi(optarg); break;
        case 'T': g_pf_only = optarg; break;
        case 'L': g_steplog = 1; break;
        case 'a': g_bfm_ack_timeout = (uint8_t)atoi(optarg); break;
        case 'Q': g_bfm_qdepth = (uint32_t)atoi(optarg); break;
        case 'v': g_bfm_verbose = 1; break;
        case 'A': fw_activate = 1; break;
        default: usage(); return 2;
        }
    }
    if (optind < argc && strcmp(argv[optind], "crc") == 0) return run_crc(iters);   /* 相手も NIC も要らない */
    if (optind + 1 >= argc) { usage(); return 2; }
    const char *mode = argv[optind];
    if (parse_ip(argv[optind + 1], &g_bfm_peer_ip[0]) != 0) { usage(); return 2; }
    const int fw = (strcmp(mode, "fw") == 0);
    const char *fw_path = NULL;
    if (fw) {
        /* fw <相手のIP> <像のファイル>: ポート 0 だけを立ち上げる(接続は 1 本。相手がいなくてもよい)*/
        if (optind + 2 >= argc) { usage(); return 2; }
        fw_path = argv[optind + 2];
        g_bfm_port_conn[0] = 1;
        g_bfm_port_conn[1] = 0;
    } else if (optind + 2 < argc) {
        /* ポート 1 は、その相手の IP を書いたときだけ使う */
        if (parse_ip(argv[optind + 2], &g_bfm_peer_ip[1]) != 0) { usage(); return 2; }
    } else {
        g_bfm_port_conn[1] = 0;
    }
    /* -c は使う全部のポートの本数。ポート 1 は、その相手の IP を書いたときだけ使う */
    if (nconn != 0 && !fw) g_bfm_port_conn[0] = nconn;
    if (optind + 2 < argc && !fw) g_bfm_port_conn[1] = nconn ? nconn : (g_bfm_port_conn[1] ? g_bfm_port_conn[1] : g_bfm_port_conn[0]);

    setvbuf(stdout, NULL, _IOLBF, 0);    /* ファイルへ出すときも 1 行ずつ出す(落ちたときに表示を失わない)*/
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGQUIT, on_sig);

    const int bench = (strcmp(mode, "bench") == 0);
    g_serve = (strcmp(mode, "serve") == 0);
    if (bench) {
        g_bfm_stage_size = BENCH_QD_MAX * BENCH_SIZE_MAX;   /* 多重度ぶんの着地点 / 送り元 */
        g_bfm_port_conn[0] = 1;                             /* 中継バッファが大きいので接続は 1 本だけ */
        g_bfm_port_conn[1] = 0;
    }
    const int race = (strcmp(mode, "race") == 0);
    const int wpost = (strcmp(mode, "wpost") == 0);
    const int perf = (strcmp(mode, "perf") == 0);
    if (perf) {
        g_bfm_async_buf_size = PERF_BIG;                   /* bfm_read_async() の 32KiB を打てるように */
        for (unsigned p = 0; p < BFM_MAX_PORT; p++)        /* 多重度の枠の中継バッファが大きいので、接続は使う分だけ */
            if (g_bfm_port_conn[p] != 0) g_bfm_port_conn[p] = PERF_CORES;
    }
    if (strcmp(mode, "run") != 0 && !g_serve && !bench && !race && !wpost && !perf && !fw && strcmp(mode, "info") != 0) {
        usage();
        return 2;
    }
    printf("%s: 接続 %u 本(ポート 0: %u 本、ポート 1: %u 本)\n", mode, conn_total(), g_bfm_port_conn[0],
           g_bfm_port_conn[1]);

    /* 最初の bfm_run() で公開メモリが張られる。相手が繋いでくる前に自分の公開メモリを埋める
     * (ポートの立ち上げには何秒もかかるので、1 回目の呼び出しの後で間に合う) */
    const int rc0 = bfm_run();
    if (rc0 < 0 && rc0 != BFM_E_STATE) {
        do_shutdown();
        return 1;
    }
    if (g_bfm_mr_size < SYNC_OFF + 8u) {
        printf("公開メモリが小さすぎる(%u バイト要る)\n", SYNC_OFF + 8u);
        do_shutdown();
        return 1;
    }
    fill_area();

    int ret;
    if (g_serve) ret = run_serve(threads);
    else if (bench) ret = run_bench(threads, iters ? iters : 100000u);
    else if (strcmp(mode, "info") == 0) ret = run_info();
    else if (race) ret = run_race(threads, iters);
    else if (wpost) ret = run_wpost(iters);
    else if (perf) ret = run_perf(iters);
    else if (fw) ret = run_fw(fw_path, fw_activate);
    else ret = run_all(threads, iters, oob);
    do_shutdown();
    if (g_steplog) {
        for (unsigned p = 0; p < BFM_MAX_PORT; p++)
            for (unsigned i = 0; i < g_bfm_port_conn[p]; i++) (void)bfm_conn_steplog(p, i);
    }
    return ret;
}
