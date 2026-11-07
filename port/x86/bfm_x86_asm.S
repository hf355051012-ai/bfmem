/* x86-64 の命令 1 つずつの関数(System V AMD64 の呼び出し規約)。宣言は bfm_x86.h。
 *
 * 引数: 1 番目 = edi / rdi、2 番目 = esi / rsi、3 番目 = rdx。戻り値 = rax。
 * rbx は呼ばれた側が保存する(cpuid が壊すので積んでおく)。 */

    .text

/* uint64_t bfm_x86_rdtsc(void) */
    .globl  bfm_x86_rdtsc
    .type   bfm_x86_rdtsc, @function
bfm_x86_rdtsc:
    rdtsc                       /* edx:eax = TSC(rax・rdx の上位 32bit は 0 になる) */
    shlq    $32, %rdx
    orq     %rdx, %rax
    ret
    .size   bfm_x86_rdtsc, . - bfm_x86_rdtsc

/* void bfm_x86_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) */
    .globl  bfm_x86_cpuid
    .type   bfm_x86_cpuid, @function
bfm_x86_cpuid:
    pushq   %rbx
    movq    %rdx, %r8           /* cpuid が edx を壊すので out を逃がす */
    movl    %edi, %eax
    movl    %esi, %ecx
    cpuid
    movl    %eax, 0(%r8)
    movl    %ebx, 4(%r8)
    movl    %ecx, 8(%r8)
    movl    %edx, 12(%r8)
    popq    %rbx
    ret
    .size   bfm_x86_cpuid, . - bfm_x86_cpuid

/* uint64_t bfm_x86_rdmsr(uint32_t msr) */
    .globl  bfm_x86_rdmsr
    .type   bfm_x86_rdmsr, @function
bfm_x86_rdmsr:
    movl    %edi, %ecx
    rdmsr                       /* edx:eax = MSR[ecx] */
    shlq    $32, %rdx
    orq     %rdx, %rax
    ret
    .size   bfm_x86_rdmsr, . - bfm_x86_rdmsr

/* void bfm_x86_wrmsr(uint32_t msr, uint64_t val) */
    .globl  bfm_x86_wrmsr
    .type   bfm_x86_wrmsr, @function
bfm_x86_wrmsr:
    movl    %edi, %ecx
    movl    %esi, %eax          /* 下位 32bit */
    movq    %rsi, %rdx
    shrq    $32, %rdx           /* 上位 32bit */
    wrmsr                       /* MSR[ecx] = edx:eax */
    ret
    .size   bfm_x86_wrmsr, . - bfm_x86_wrmsr

/* void bfm_x86_pause(void) */
    .globl  bfm_x86_pause
    .type   bfm_x86_pause, @function
bfm_x86_pause:
    pause
    ret
    .size   bfm_x86_pause, . - bfm_x86_pause

#if defined(__linux__)
    .section .note.GNU-stack, "", @progbits    /* スタックを実行可能にしない */
#endif
