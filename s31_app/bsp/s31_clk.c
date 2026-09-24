/*===========================================================================
 * s31_clk.c -- 时基与延时（SYSTIMER @16MHz，与 CPU 频率无关）
 *
 * SYSTIMER 的时钟固定 XTAL 40MHz / 2.5 = 16MHz
 * （IDF esp_hw_support/port/esp32s31/systimer.c 注释 + rtt 工程用 PC 墙钟校过）。
 * 拿它当延时基准的好处：CPU 提频（40→320MHz）后延时不用重新标定。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"

void s31_systimer_init(void)
{
    /* 1. 时钟门控（hp_sys_clkrst_reg.h）：bit0 APB_CLK_EN、bit4 CLK_EN */
    S31_REG32(S31_SYSTIMER_CTRL0) |= (S31_SYSTIMER_APB_CLK_EN | S31_SYSTIMER_CLK_EN_BIT);

    /* 2. 寄存器时钟 + unit0 工作 */
    S31_REG32(S31_SYSTIMER_CONF) |= (S31_SYSTIMER_CLK_EN | S31_SYSTIMER_UNIT0_WORK_EN);

    /* 3. 计数器清零（LOAD_HI/LO → LOAD 生效）*/
    S31_REG32(S31_SYSTIMER_UNIT0_LOAD_HI) = 0;
    S31_REG32(S31_SYSTIMER_UNIT0_LOAD_LO) = 0;
    S31_REG32(S31_SYSTIMER_UNIT0_LOAD) = 1;
}

/* 读计数器：必须走 UPDATE/VALUE_VALID 握手 + 读两遍防撕裂。
 * 🚨 等待必须有上限：时钟门控没开时 VALUE_VALID 永远不置位，
 *    无上限等待会把系统挂死在这儿（rtt 工程踩过）。 */
uint64_t s31_systimer_ticks(void)
{
    uint32_t hi, lo, lo2;
    volatile uint32_t guard = 200000u;

    S31_REG32(S31_SYSTIMER_UNIT0_OP) = S31_SYSTIMER_UNIT0_OP_UPDATE;
    while (!(S31_REG32(S31_SYSTIMER_UNIT0_OP) & S31_SYSTIMER_UNIT0_VALUE_VALID) && --guard) {
    }
    if (guard == 0) {
        return 0;
    }
    do {
        lo  = S31_REG32(S31_SYSTIMER_UNIT0_VALUE_LO);
        hi  = S31_REG32(S31_SYSTIMER_UNIT0_VALUE_HI);
        lo2 = S31_REG32(S31_SYSTIMER_UNIT0_VALUE_LO);
    } while (lo != lo2);

    return ((uint64_t)hi << 32) | lo;
}

uint32_t s31_millis(void)
{
    return (uint32_t)(s31_systimer_ticks() / (S31_SYSTIMER_HZ / 1000u));
}

void s31_delay_us(uint32_t us)
{
    uint64_t start = s31_systimer_ticks();
    uint64_t need = (uint64_t)us * (S31_SYSTIMER_HZ / 1000000u);
    while ((s31_systimer_ticks() - start) < need) {
    }
}

/* 忙等时顺便把控制台发送缓冲灌出去。
 * 🚨 这不是"顺手优化"：一旦固件卡在某个死等循环里（比如 USB 驱动等控制器就绪），
 *    环形缓冲里没送出去的那几行日志就永远看不到了 —— 排查时只能看到半截日志。
 *    把 pump 放进延时里，任何忙等都会持续刷控制台。 */
void s31_usj_pump(void);

void s31_delay_ms(uint32_t ms)
{
    while (ms--) {
        s31_delay_us(1000);
        s31_usj_pump();
    }
}

/*===========================================================================
 * 整片复位：走 TIMG0 的 MWDT（stage0 = RESET_SYSTEM）
 *
 * 为什么不用别的方式（rtt 工程三条死路都踩过）：
 *   ① LP_AONCLKRST 的 core0 软复位（IDF esp_restart 用的那位）→ core0 被永久
 *      按在复位里，AON 域复位不清，只能物理上电；
 *   ② RTC_WDT → 我们没配 RTC 慢时钟域，计数器不走，等不到复位；
 *   ③ 关掉超级看门狗等它复位 → 也不生效。
 * TIMG0 的 MWDT 时钟是 HP 域 APB，肯定在走，几百微秒就复位。
 *===========================================================================*/
void s31_system_reset(void)
{
    S31_REG32(S31_TIMG0_BASE + S31_TIMG_WDTWPROTECT) = S31_WDT_WKEY;
    S31_REG32(S31_TIMG0_BASE + S31_TIMG_WDTCONFIG2) = 20000u;   /* 预分频 */
    S31_REG32(S31_TIMG0_BASE + S31_TIMG_WDTCONFIG1) = 200u;     /* stg0 计数 */
    S31_REG32(S31_TIMG0_BASE + S31_TIMG_WDTCONFIG0) = (1u << 31) | (3u << 29);

    for (;;) {
        s31_usj_pump();
    }
}
