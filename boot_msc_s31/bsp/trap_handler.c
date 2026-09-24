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

/*===========================================================================
 * "可恢复探测"守卫（g_probe_guard）
 *
 * 用途：PSRAM 探测这种"**明确预期可能失败**"的动作。裸机里没有 MMU 保护，
 *   一次非法访问就是异常，而异常默认会进停机循环 —— 结果是**探测失败 = 板子失联**
 *   （实测踩过：MMU 映射没生效时写 0x50000000 触发 mcause=7，
 *    bootloader 再也起不来、要物理断电）。
 *
 * 做法：探测期间置 g_probe_guard=1。异常进来时若它是**访问异常**且处于守卫期，
 *   就**跳过那条指令**（按 RISC-V 指令长度：低 2 位 == 3 则为 4 字节，否则 2 字节），
 *   计入 g_probe_faults 后正常返回继续跑。
 *
 * 为什么是"跳过指令"而不是"longjmp 回恢复点"：
 *   跳过不需要恢复任何寄存器 —— 编译器可能把状态放在任意寄存器里，
 *   手工 longjmp 很容易留下不一致状态。配合"有界循环"（探测都是几十次迭代），
 *   跳过足够安全，且行为可预测（探测结果 = 全部不匹配 -> 判失败）。
 *===========================================================================*/
volatile int g_probe_guard;
volatile int g_probe_faults;

static void s31_exception(uint32_t mcause, uint32_t mepc)
{
    uint32_t mtval, mstatus;
    /* 🚨 异常号必须**掩码后再比**！实测 mcause=0x30000007 —— 高位的 0x30000000
       是 CLIC 的附加信息，异常号只在低位。直接 `mcause == 7` 永远不成立，
       于是守卫形同虚设、掉进停机分支（踩过：烧进去 0 字节输出，白折腾一轮）。*/
    uint32_t code = mcause & 0xFu;

    __asm__ volatile ("csrr %0, mtval"   : "=r"(mtval));
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));

    /* 守卫期内 + 访问异常（1=取指 5=load 7=store）或非法指令（2，
       读未实现的 CSR 会走这条）-> 跳过该指令 */
    if (g_probe_guard && (code == 1u || code == 2u || code == 5u || code == 7u)) {
        uint16_t insn = *(volatile uint16_t *)mepc;
        uint32_t len = ((insn & 0x3u) == 0x3u) ? 4u : 2u;
        g_probe_faults++;
        if (g_probe_faults <= 2) {           /* 只报前两次，别刷屏 */
            kprintf("!! probe guard: code=%u addr=0x%08x mepc=0x%08x -> 跳过 %u 字节\r\n",
                    (unsigned)code, (unsigned)mtval, (unsigned)mepc, (unsigned)len);
        }
        __asm__ volatile ("csrw mepc, %0" : : "r"(mepc + len));
        return;
    }

    kprintf("\r\n*** S31 EXCEPTION ***\r\n");
    kprintf("  mcause =0x%08x (code=%u)  mepc=0x%08x\r\n",
            (unsigned)mcause, (unsigned)code, (unsigned)mepc);
    kprintf("  mtval  =0x%08x  mstatus=0x%08x\r\n", (unsigned)mtval, (unsigned)mstatus);
    kprintf("(halted)\r\n");

    /* 🚨 停机循环里必须继续刷发送缓冲：最后几条日志很可能还压在环形缓冲里
     *    （FIFO 只有 64 字节），不刷就永远看不到现场 —— 这个坑踩过。
     *   ⚠️ 但也**不能刷太猛**：曾经因为死循环里毫无间隔地灌 USB FIFO，
     *   结果 Windows 直接报"设备不识别"、COM43 打开就报 PermissionError，
     *   只能靠 JTAG `reset run` 救回来。所以加一个粗延时。*/
    for (;;) {
        s31_usj_pump();
        for (volatile uint32_t d = 0; d < 40000u; d++) {
            /* 空转，别让 USB 外设被灌爆 */
        }
    }
}

void s31_trap_dispatch(uint32_t mcause, uint32_t mepc)
{
    if (mcause & 0x80000000u) {                 /* 中断 */
        uint32_t id = mcause & 0xFFFu;
        if (id < S31_ISR_MAX && s_isr_table[id]) {
            s_isr_table[id]((int)id, s_isr_arg[id]);
        } else {
            s31_default_isr((int)id, 0);
        }
    } else {
        s31_exception(mcause, mepc);            /* 注意：要用参数里的 mepc */
    }
}
