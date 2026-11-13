/* x86-64 の CPU を直接触る部品: TSC の周波数を決め、TSC で時刻と待ちを作る。
 * CPU の周波数などを MSR で読んで表示する。OS の機能は使わない(表示は bfm_port_log)。
 *
 * 存在しない MSR を読むと #GP で落ちる。読む前に CPUID で確かめ、機種ごとの MSR
 * (MSR_PLATFORM_INFO など)は Intel の family 6(Nehalem 以降の Core / Xeon)のときだけ読む。 */
#include "bfm_x86.h"
#include "bfm_port.h"
#include "bfm_config.h"

#include <string.h>

/* MSR の番号(Intel SDM Vol.4) */
#define MSR_PLATFORM_INFO     0xCEu    /* [15:8] 基準の倍率 [47:40] 最小の倍率(機種ごと) */
#define MSR_PKG_CST_CONFIG    0xE2u    /* [3:0] パッケージ C-state の上限 [15] 固定(機種ごと) */
#define IA32_MPERF            0xE7u    /* 基準の周波数で数える(C0 の間だけ) */
#define IA32_APERF            0xE8u    /* 実際の周波数で数える(C0 の間だけ) */
#define IA32_PERF_STATUS      0x198u   /* [15:8] 今の倍率 */
#define MSR_TURBO_RATIO_LIMIT 0x1ADu   /* [7:0] 1 コアだけ動くときの最大の倍率(機種ごと) */

#define BUS_HZ 100000000ull            /* 倍率の単位(Sandy Bridge 以降は 100MHz) */

static bfm_x86_msr_reader s_rdmsr;     /* NULL = MSR を読まない */
static uint64_t s_tsc_hz;              /* TSC の周波数(Hz)。0 = 求められなかった */
static uint64_t s_ns_mult;             /* ns = TSC * s_ns_mult >> 32 */
static const char *s_tsc_src = "";    /* TSC の周波数をどこから求めたか(表示用)*/
static uint32_t s_max_leaf;            /* CPUID の基本の葉の最大番号 */
static uint32_t s_max_ext;             /* CPUID の拡張の葉(0x8000xxxx)の最大番号 */
static uint32_t s_family;              /* CPU のファミリ(拡張ファミリを足したもの)*/
static uint32_t s_model;               /* CPU のモデル(拡張モデルを足したもの)*/
static uint32_t s_leaf1_ecx;           /* CPUID 1 の ecx(bit7 = EIST)*/
static uint32_t s_leaf6_eax;           /* CPUID 6 の eax(bit1 = ターボ)*/
static uint32_t s_leaf6_ecx;           /* CPUID 6 の ecx(bit0 = APERF / MPERF)*/
static int s_intel;                    /* Intel の CPU(MSR はこのときだけ読む)*/
static int s_invariant;                /* TSC の速さが P-state・C-state で変わらない */
static char s_brand[49];               /* CPU の名前(CPUID 0x80000002〜4。表示用)*/

/* CPUID で CPU の種類と、使える機能を調べる。 */
static void cpu_probe(void)
{
    uint32_t r[4];
    uint32_t i;

    bfm_x86_cpuid(0, 0, r);
    s_max_leaf = r[0];
    /* ベンダ名は ebx・edx・ecx の順に "Genu" "ineI" "ntel" */
    s_intel = r[1] == 0x756e6547u && r[3] == 0x49656e69u && r[2] == 0x6c65746eu;

    bfm_x86_cpuid(1, 0, r);
    s_family = (r[0] >> 8) & 0xfu;
    s_model = (r[0] >> 4) & 0xfu;
    if (s_family == 0xfu) s_family += (r[0] >> 20) & 0xffu;
    if (s_family == 6u || s_family == 0xfu) s_model |= ((r[0] >> 16) & 0xfu) << 4;
    s_leaf1_ecx = r[2];

    s_leaf6_eax = s_leaf6_ecx = 0;
    if (s_max_leaf >= 6u) {
        bfm_x86_cpuid(6, 0, r);
        s_leaf6_eax = r[0];
        s_leaf6_ecx = r[2];
    }

    bfm_x86_cpuid(0x80000000u, 0, r);
    s_max_ext = r[0];
    s_invariant = 0;
    if (s_max_ext >= 0x80000007u) {
        bfm_x86_cpuid(0x80000007u, 0, r);
        s_invariant = (r[3] >> 8) & 1u;      /* edx[8]: P-state・C-state で TSC の速さが変わらない */
    }
    s_brand[0] = '\0';
    if (s_max_ext >= 0x80000004u) {
        for (i = 0; i < 3u; i++) {
            bfm_x86_cpuid(0x80000002u + i, 0, r);
            memcpy(s_brand + 16u * i, r, 16u);
        }
        s_brand[48] = '\0';
    }
}

/* MSR を読む。読めない(読む関数が無い・Intel の family 6 でない)なら -1。 */
static int msr_read(uint32_t msr, uint64_t *val)
{
    if (s_rdmsr == NULL || !s_intel || s_family != 6u) return -1;
    return s_rdmsr(msr, val);
}

/* TSC の周波数を決める(0 = 分からない)。 */
static uint64_t tsc_hz_find(void)
{
    uint32_t r[4];
    uint64_t v;

    if (g_bfm_tsc_hz != 0) {
        s_tsc_src = "g_bfm_tsc_hz";
        return g_bfm_tsc_hz;
    }
    if (s_max_leaf >= 0x15u) {
        bfm_x86_cpuid(0x15, 0, r);           /* TSC = 水晶(ecx Hz)x ebx / eax */
        if (r[0] != 0 && r[1] != 0 && r[2] != 0) {
            s_tsc_src = "CPUID 15h";
            return (uint64_t)r[2] * r[1] / r[0];
        }
    }
    if (s_max_leaf >= 0x16u) {
        bfm_x86_cpuid(0x16, 0, r);           /* eax[15:0] = 基準の周波数(MHz)。TSC はこの速さ */
        if ((r[0] & 0xffffu) != 0) {
            s_tsc_src = "CPUID 16h";
            return (uint64_t)(r[0] & 0xffffu) * 1000000u;
        }
    }
    if (msr_read(MSR_PLATFORM_INFO, &v) == 0 && ((v >> 8) & 0xffu) != 0) {
        s_tsc_src = "MSR_PLATFORM_INFO";
        return ((v >> 8) & 0xffu) * BUS_HZ;
    }
    return 0;
}

int bfm_x86_init(bfm_x86_msr_reader rdmsr)
{
    s_rdmsr = rdmsr;
    cpu_probe();
    s_tsc_hz = tsc_hz_find();
    if (s_tsc_hz == 0) {
        bfm_port_log("x86: TSC の周波数が分からない(g_bfm_tsc_hz に入れること)\n");
        return -1;
    }
    s_ns_mult = (uint64_t)(((unsigned __int128)1000000000u << 32) / s_tsc_hz);
    if (!s_invariant) {
        bfm_port_log("x86: 注意: TSC が invariant でない(省電力で TSC の速さが変わり、時間切れの判定がずれうる)\n");
    }
    if (g_bfm_verbose) bfm_x86_report();
    return 0;
}

uint64_t bfm_x86_time_ns(void)
{
    return (uint64_t)(((unsigned __int128)bfm_x86_rdtsc() * s_ns_mult) >> 32);
}

uint32_t bfm_x86_apic_id(void)
{
    uint32_t r[4];
    uint32_t max = s_max_leaf;
    if (max == 0u) {                       /* bfm_x86_init() の前 */
        bfm_x86_cpuid(0, 0, r);
        max = r[0];
    }
    if (max >= 0x0bu) {
        bfm_x86_cpuid(0x0b, 0, r);
        if (r[1] != 0u) return r[3];       /* ebx != 0 なら 0Bh がある。edx = x2APIC ID */
    }
    bfm_x86_cpuid(1, 0, r);
    return r[1] >> 24;
}

void bfm_x86_delay_us(uint32_t us)
{
    const uint64_t t0 = bfm_x86_rdtsc();
    const uint64_t n = (uint64_t)us * s_tsc_hz / 1000000u;
    while (bfm_x86_rdtsc() - t0 < n) bfm_x86_pause();
}

/* Hz を「1234.567 MHz」で表示するための分け方 */
#define MHZ(hz) (unsigned long long)((hz) / 1000000u), (unsigned long long)((hz) / 1000u % 1000u)

void bfm_x86_report(void)
{
    const char *b = s_brand;
    uint64_t v, a0, m0, a1, m1;

    while (*b == ' ') b++;
    bfm_port_log("x86: %s(%s family 0x%x model 0x%x)\n", *b ? b : "(名前なし)",
                 s_intel ? "Intel" : "Intel 以外", s_family, s_model);
    bfm_port_log("x86: TSC %llu.%03llu MHz(%s)、invariant %s\n", MHZ(s_tsc_hz), s_tsc_src,
                 s_invariant ? "である" : "でない");
    if (s_rdmsr == NULL) {
        bfm_port_log("x86: MSR は読まない\n");
        return;
    }
    if (msr_read(MSR_PLATFORM_INFO, &v) == 0) {
        bfm_port_log("x86: 基準 %llu MHz(倍率 %u)、最小 %llu MHz\n",
                     (unsigned long long)(((v >> 8) & 0xffu) * 100u), (unsigned)((v >> 8) & 0xffu),
                     (unsigned long long)(((v >> 40) & 0xffu) * 100u));
    }
    if ((s_leaf6_eax & 2u) != 0 && msr_read(MSR_TURBO_RATIO_LIMIT, &v) == 0) {   /* eax[1]: ターボがある */
        bfm_port_log("x86: ターボの最大(1 コア)%llu MHz\n", (unsigned long long)((v & 0xffu) * 100u));
    }
    if ((s_leaf1_ecx & 0x80u) != 0 && msr_read(IA32_PERF_STATUS, &v) == 0) {     /* ecx[7]: EIST */
        bfm_port_log("x86: 今の倍率 %u(%llu MHz)\n", (unsigned)((v >> 8) & 0xffu),
                     (unsigned long long)(((v >> 8) & 0xffu) * 100u));
    }
    /* 実効の周波数: 10ms 空回りする間の APERF / MPERF の増え方の比 x 基準(= TSC)の周波数 */
    if ((s_leaf6_ecx & 1u) != 0 &&                                                  /* ecx[0]: APERF / MPERF */
        msr_read(IA32_MPERF, &m0) == 0 && msr_read(IA32_APERF, &a0) == 0) {
        bfm_x86_delay_us(10000u);
        if (msr_read(IA32_MPERF, &m1) == 0 && msr_read(IA32_APERF, &a1) == 0 && m1 != m0) {
            const uint64_t hz = (uint64_t)((unsigned __int128)s_tsc_hz * (a1 - a0) / (m1 - m0));
            bfm_port_log("x86: 実効 %llu.%03llu MHz(APERF / MPERF、10ms 空回りした間)\n", MHZ(hz));
        }
    }
    if (msr_read(MSR_PKG_CST_CONFIG, &v) == 0) {
        bfm_port_log("x86: パッケージ C-state の上限 %u(符号は機種ごと)%s\n", (unsigned)(v & 0xfu),
                     (v & 0x8000u) ? "、固定されている" : "");
    }
}
