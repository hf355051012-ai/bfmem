#ifndef BFM_X86_H
#define BFM_X86_H

/* =====================================================================
 * x86-64 の CPU を直接触る部品(移植層から使う。OS の機能は使わない)。
 *
 *   bfm_x86_asm.S  命令 1 つずつの関数(rdtsc・cpuid・rdmsr・wrmsr・pause)
 *   bfm_x86.c      TSC の周波数を求め、TSC で時刻と待ちを作る。CPU の周波数を表示する。
 *
 * rdmsr / wrmsr は特権命令(リング 0)。VxWorks のカーネル(DKM)のタスクからは直接使えるが、
 * Linux のユーザ空間では使えない(Linux の移植層は /dev/cpu/N/msr で読む)。
 * 存在しない MSR を読むと #GP で落ちるので、bfm_x86.c は CPUID で確かめてから読む。
 * ===================================================================== */

#include <stdint.h>

/* ---- bfm_x86_asm.S(命令そのもの) ---- */

/* TSC を読む(rdtsc)。 */
uint64_t bfm_x86_rdtsc(void);
/* CPUID(eax = leaf, ecx = sub)。out[0..3] = eax, ebx, ecx, edx。 */
void     bfm_x86_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]);
/* MSR を読む(rdmsr。リング 0 でだけ使える)。 */
uint64_t bfm_x86_rdmsr(uint32_t msr);
/* MSR に書く(wrmsr。リング 0 でだけ使える)。 */
void     bfm_x86_wrmsr(uint32_t msr, uint64_t val);
/* 待ちのループの中で呼ぶ(pause)。 */
void     bfm_x86_pause(void);

/* ---- bfm_x86.c ---- */

/* MSR を読む関数(移植層が渡す)。0 = 読めた。 */
typedef int (*bfm_x86_msr_reader)(uint32_t msr, uint64_t *val);

/* CPU を調べて TSC の周波数を決める。rdmsr は MSR を読む関数(NULL = MSR を使わない)。
 * 周波数は g_bfm_tsc_hz → CPUID 15h → CPUID 16h → MSR_PLATFORM_INFO の順に決める。
 * g_bfm_verbose なら CPU の名前・TSC・周波数を表示する。0 = 成功、-1 = TSC の周波数が分からない。 */
int      bfm_x86_init(bfm_x86_msr_reader rdmsr);
/* TSC から作った時刻(ナノ秒)。bfm_x86_init() の後で使う。 */
uint64_t bfm_x86_time_ns(void);
/* TSC を見ながら空回りで待つ(マイクロ秒)。 */
void     bfm_x86_delay_us(uint32_t us);
/* CPU の名前・TSC・周波数(基準・最大・今の実効)・パッケージ C-state の設定を表示する。 */
void     bfm_x86_report(void);

#endif /* BFM_X86_H */
