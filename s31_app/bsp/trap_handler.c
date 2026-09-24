/*===========================================================================
 * trap_handler.c -- CLIC 中断分发 + 异常现场打印
 *
 * mcause 编码（CLIC）：
 *   bit31 = 1 中断 / 0 异常
 *   reason[11:0] = 中断号（对 CLIC 来说就是 CLIC ID）
 *
 * 中断表按 CLIC ID 索引，容量 64（S31 外部中断从 16 号起）。
 * 未注册的中断：打几行日志后**把该路 CLIC IE 关掉** —— 只打日志不清标志的话，
 * 电平触发下会立刻反复进来，把整个系统刷死（rtt 工程实测被刷过 1.2MB 日志）。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"

#define S31_ISR_MAX 64

typedef void (*s31_isr_t)(int id, void *arg);

static s31_isr_t s_isr_table[S31_ISR_MAX];
static void     *s_isr_arg[S31_ISR_MAX];

int kprintf(const char *fmt, ...);
void s31_usj_pump(void);

void s31_irq_install(int id, s31_isr_t fn, void *arg)
{
    if (id >= 0 && id < S31_ISR_MAX) {
        s_isr_table[id] = fn;
        s_isr_arg[id]   = arg;
    }
}

static void s31_default_isr(int id, void *arg)
{
    static uint32_t spammed;
    (void)arg;

    if (spammed < 3u) {
        kprintf("!! unhandled CLIC interrupt id=%d -> disabled\r\n", id);
    }
    spammed++;
    if (id >= S31_CLIC_EXT_OFFSET && id < S31_ISR_MAX) {
        S31_REG8(S31_CLIC_IE(id)) = 0;      /* 关掉这一路，让系统活下来 */
    }
}

static void s31_exception(uint32_t mcause)
{
    uint32_t mepc, mtval, mstatus;

    __asm__ volatile ("csrr %0, mepc"    : "=r"(mepc));
    __asm__ volatile ("csrr %0, mtval"   : "=r"(mtval));
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));

    kprintf("\r\n*** S31 EXCEPTION ***\r\n");
    kprintf("  mcause =0x%08x  mepc=0x%08x\r\n", (unsigned)mcause, (unsigned)mepc);
    kprintf("  mtval  =0x%08x  mstatus=0x%08x\r\n", (unsigned)mtval, (unsigned)mstatus);
    kprintf("(halted)\r\n");

    /* 🚨 停机循环里必须继续刷发送缓冲：最后几条日志很可能还压在环形缓冲里
     *    （FIFO 只有 64 字节），不刷就永远看不到现场 —— 这个坑踩过。 */
    for (;;) {
        s31_usj_pump();
    }
}

void s31_trap_dispatch(uint32_t mcause, uint32_t mepc)
{
    (void)mepc;

    if (mcause & 0x80000000u) {                 /* 中断 */
        uint32_t id = mcause & 0xFFFu;
        if (id < S31_ISR_MAX && s_isr_table[id]) {
            s_isr_table[id]((int)id, s_isr_arg[id]);
        } else {
            s31_default_isr((int)id, 0);
        }
    } else {
        s31_exception(mcause);
    }
}
