/*
 * s31_cpuclk.c -- 把 CPU 从 bootROM 交棒时的 XTAL 40MHz 提到 CPLL 320MHz
 *
 * 为什么需要：
 *   我们的 MSC bootloader **整个 EP0 + MSC 全在中断里做**（裸机没线程）。
 *   CPU 40MHz 时中断路径比 IDF 的 320MHz 慢 8 倍 ——
 *   IDF + CherryUSB 的 MSC 基线实测 4/4 稳定，而本固件时好时坏，
 *   这是两者最结构性的一条差别。提到 320MHz 后 USB 中断的响应余量完全不同。
 *
 * 参考来源：`projects/rtt_nano_s31/bsp/drv_clk.c`（同板裸机 RT-Thread 移植，
 *   已经把这条路走通并实测到 319~320MHz）。本文件是它的精简移植：
 *   去掉 RT-Thread 依赖、去掉打点等待逻辑，只保留"上电 CPLL → 配分频 → 切源"。
 *   顺序与 IDF `rtc_clk_cpll_configure()` 一致。
 *
 * ⚠️ 三个必须记住的点（都是参考工程踩出来的）：
 *   1. 切源靠 `ROOT_CLK_CTRL0.reg_soc_clk_update` 握手位，**必须等它自清**，
 *      而且要带超时 —— 否则系统直接挂死在切频那一步（现象：连 ROM 横幅之后都没输出）。
 *   2. 切完必须调 ROM 的 `ets_update_cpu_frequency`（0x2f800044），
 *      否则 ROM 里那套基于 CPU 频率的延时/串口分频全是错的。
 *   3. 我们的代码全都跑在 RAM（0x2F000000+）、延时用的是 SYSTIMER（固定 16MHz），
 *      所以切频期间不碰 flash、延时基准也不受影响 —— 这也是敢切的前提。
 */
#include <stdint.h>
#include "s31_regs.h"

int  kprintf(const char *fmt, ...);
void s31_usj_pump(void);
void s31_usb_set_system_clock(uint32_t hz);
uint32_t s31_cpu_mhz(void);          /* s31_clk.c：mcycle × SYSTIMER 实测 */

/*===========================================================================
 * 寄存器（HP_SYS_CLKRST @0x20587000）
 *===========================================================================*/
#define R_SOC_CLK_SEL        (S31_HP_SYS_CLKRST_BASE + 0x00u)
#define R_CPU_FREQ_CTRL0     (S31_HP_SYS_CLKRST_BASE + 0x04u)
#define R_MEM_FREQ_CTRL0     (S31_HP_SYS_CLKRST_BASE + 0x08u)
#define R_SYS_FREQ_CTRL0     (S31_HP_SYS_CLKRST_BASE + 0x0cu)
#define R_APB_FREQ_CTRL0     (S31_HP_SYS_CLKRST_BASE + 0x10u)
#define R_ROOT_CLK_CTRL0     (S31_HP_SYS_CLKRST_BASE + 0x14u)
#define R_CPU_SRC_FREQ0      (S31_HP_SYS_CLKRST_BASE + 0x168u)  /* RO，0.25MHz/step */
#define R_CPU_CLK_STATUS0    (S31_HP_SYS_CLKRST_BASE + 0x16cu)
#define R_ANA_PLL_CTRL0      (S31_HP_SYS_CLKRST_BASE + 0x174u)

#define R_PMU_HP_CK_POWER_1  (S31_PMU_BASE + 0xf4u)
#define B_TIE_HIGH_XPD_CPLL        (1u << 27)
#define B_TIE_HIGH_XPD_CPLL_I2C    (1u << 23)
#define B_TIE_HIGH_GLOBAL_CPLL_ICG (1u << 19)

#define R_HP_ALIVE_SYS_CLK_CTRL  S31_HP_ALIVE_SYS_BASE
#define B_HP_CPLL_300M_CLK_EN    (1u << 29)

#define R_LP_AON_CPLL_DIV    (S31_LP_CLKRST_BASE + 0x4cu)
#define S_CPLL_REF_DIV       0
#define S_CPLL_FB_DIV        4

#define B_ANA_PLL_CPLL_CAL_STOP  (1u << 3)
#define B_ANA_PLL_CPLL_CAL_END   (1u << 2)   /* RO */

#define B_CLK_STATUS_SRC_IS_CPLL (1u << 2)

#define M_CPU_FREQ_DIV       0x3FFu
#define M_MEM_FREQ_DIV       0x1u
#define M_SYS_FREQ_DIV       0xFFu
#define M_APB_FREQ_DIV       0xFFu

/* ROM 的 ets_update_cpu_frequency：不改它，ROM 的延时/串口分频就还是旧主频 */
#define ROM_ETS_UPDATE_CPU_FREQ  ((void (*)(uint32_t))0x2f800044u)

#define S31_CPU_TARGET_MHZ   320u

/*===========================================================================
 * 小工具
 *===========================================================================*/
/* 硬件频率回读：0.25MHz/step。比软件测量权威，切完必须复核。 */
uint32_t s31_cpu_hw_mhz(void)
{
    return S31_REG32(R_CPU_SRC_FREQ0) / 4u;
}

static int clk_on_cpll(void)
{
    return (S31_REG32(R_CPU_CLK_STATUS0) & B_CLK_STATUS_SRC_IS_CPLL) ? 1 : 0;
}

static void clk_mark(const char *s)
{
    /* 切频失败会直接没输出，所以每一步都要**立刻**送出去（pump 不阻塞，满则留到下次）*/
    kprintf("%s", s);
    s31_usj_pump();
}

static void clk_set_dividers(uint32_t cpu_div, uint32_t mem_div,
                             uint32_t sys_div, uint32_t apb_div)
{
    S31_REG32(R_CPU_FREQ_CTRL0) =
        (S31_REG32(R_CPU_FREQ_CTRL0) & ~M_CPU_FREQ_DIV) | ((cpu_div - 1u) & 0xFFu);
    S31_REG32(R_MEM_FREQ_CTRL0) =
        (S31_REG32(R_MEM_FREQ_CTRL0) & ~M_MEM_FREQ_DIV) | ((mem_div - 1u) & 0x1u);
    S31_REG32(R_SYS_FREQ_CTRL0) =
        (S31_REG32(R_SYS_FREQ_CTRL0) & ~M_SYS_FREQ_DIV) | ((sys_div - 1u) & 0xFFu);
    S31_REG32(R_APB_FREQ_CTRL0) =
        (S31_REG32(R_APB_FREQ_CTRL0) & ~M_APB_FREQ_DIV) | ((apb_div - 1u) & 0xFFu);
}

/* 切源 + 等原子生效。⚠️ 必须带超时：万一 update 位不自清，宁可退回 40MHz 也不能挂死 */
static int clk_switch_src(uint32_t sel)
{
    uint32_t guard = 200000u;

    S31_REG32(R_SOC_CLK_SEL) = (S31_REG32(R_SOC_CLK_SEL) & ~0x3u) | (sel & 0x3u);
    S31_REG32(R_ROOT_CLK_CTRL0) = 1u;                 /* reg_soc_clk_update */
    while ((S31_REG32(R_ROOT_CLK_CTRL0) & 1u) && --guard) {
    }
    if (guard == 0u) {
        kprintf("\r\n[clk] !! 切源握手位不自清 (root=%08x sel=%08x)\r\n",
                (unsigned)S31_REG32(R_ROOT_CLK_CTRL0), (unsigned)S31_REG32(R_SOC_CLK_SEL));
        return -1;
    }
    return 0;
}

/* CPLL 上电 + 配 320MHz + 自校准。顺序照 IDF rtc_clk_cpll_configure() */
static int cpll_setup_320m(void)
{
    uint32_t guard;
    uint32_t fb_ref;

    clk_mark("[clk] 1 模拟时钟\r\n");
    /* 前置：MODEM 寄存器总线时钟 + regi2c 主时钟（IDF 的 ANALOG_CLOCK_ENABLE）*/
    S31_REG32(S31_HP_SYS_CLKRST_BASE + 0x40u) |= (1u << 0);
    S31_REG32(S31_MODEM_LPCON_BASE + 0x18u) |= (1u << 2);

    clk_mark("[clk] 2 CPLL 上电\r\n");
    /* XPD -> XPD_I2C -> GLOBAL_ICG（顺序照 IDF）*/
    S31_REG32(R_PMU_HP_CK_POWER_1) |= B_TIE_HIGH_XPD_CPLL;
    S31_REG32(R_PMU_HP_CK_POWER_1) |= B_TIE_HIGH_XPD_CPLL_I2C;
    S31_REG32(R_PMU_HP_CK_POWER_1) |= B_TIE_HIGH_GLOBAL_CPLL_ICG;
    S31_REG32(R_HP_ALIVE_SYS_CLK_CTRL) |= B_HP_CPLL_300M_CLK_EN;

    clk_mark("[clk] 3 倍频 40x8/1\r\n");
    /* 40MHz × 8 / 1 = 320MHz（ref_div=1, fb_div=8）*/
    fb_ref = S31_REG32(R_LP_AON_CPLL_DIV);
    fb_ref &= ~((0xFu << S_CPLL_REF_DIV) | (0xFFu << S_CPLL_FB_DIV));
    fb_ref |= (1u << S_CPLL_REF_DIV) | (8u << S_CPLL_FB_DIV);
    S31_REG32(R_LP_AON_CPLL_DIV) = fb_ref;

    clk_mark("[clk] 4 校准\r\n");
    S31_REG32(R_ANA_PLL_CTRL0) &= ~B_ANA_PLL_CPLL_CAL_STOP;
    guard = 200000u;
    while (!(S31_REG32(R_ANA_PLL_CTRL0) & B_ANA_PLL_CPLL_CAL_END) && --guard) {
    }
    if (guard == 0u) {
        kprintf("\r\n[clk] !! CPLL 校准超时 (ana=%08x) -> 不切源\r\n",
                (unsigned)S31_REG32(R_ANA_PLL_CTRL0));
        return -1;
    }
    for (guard = 0; guard < 400u; guard++) {          /* ~10us 忙等（照 IDF）*/
    }
    S31_REG32(R_ANA_PLL_CTRL0) |= B_ANA_PLL_CPLL_CAL_STOP;
    return 0;
}

/* 显式退回 XTAL 40MHz。
   ⚠️ 注意：**时钟配置跨复位保留** —— CPLL 切过去之后热复位不会自己退回 40MHz。
   所以"想在 40MHz 下做某件事"必须显式切回来，光靠复位是没用的（踩过：A/B 实验白做一轮）。 */
void s31_cpu_clk_to_xtal(void)
{
    clk_set_dividers(1, 1, 1, 1);
    (void)clk_switch_src(0);                          /* 0 = XTAL */
    ROM_ETS_UPDATE_CPU_FREQ(40u);
    s31_usb_set_system_clock(40000000u);
}

/*===========================================================================
 * 对外入口
 *
 * 分频表（IDF rtc_clk.c 的约束：MEM≤160M、SYS≤320/3、APB≤320/6）：
 *   320MHz: cpu÷1 mem÷2(160M) sys÷3(106.7M) apb÷2(53.3M)
 *   160MHz: cpu÷2 mem÷1(160M) sys÷2(80M)    apb÷2(40M)
 *    80MHz: cpu÷4 mem÷1(80M)  sys÷1(80M)    apb÷2(40M)
 *===========================================================================*/
int s31_cpu_clk_to_mhz(uint32_t target_mhz)
{
    uint32_t cpu_div, mem_div, sys_div, apb_div;

    switch (target_mhz) {
    case 320: cpu_div = 1; mem_div = 2; sys_div = 3; apb_div = 2; break;
    case 160: cpu_div = 2; mem_div = 1; sys_div = 2; apb_div = 2; break;
    case 80:  cpu_div = 4; mem_div = 1; sys_div = 1; apb_div = 2; break;
    default:  return -1;
    }

    if (clk_on_cpll()) {
        /* 已经在 CPLL 上（例如 IDF 的 bootloader 配过 CPLL/4=80MHz）：
           只改分频即可，不需要切源。**注意别像参考工程那样直接 return** ——
           那样会停在 80MHz 却对外声称 320MHz。 */
        clk_set_dividers(cpu_div, mem_div, sys_div, apb_div);
        ROM_ETS_UPDATE_CPU_FREQ(target_mhz);
        return 0;
    }

    if (cpll_setup_320m() != 0) {
        s31_cpu_clk_to_xtal();
        return -1;
    }
    clk_set_dividers(cpu_div, mem_div, sys_div, apb_div);
    clk_mark("[clk] 5 分频已设\r\n");
    if (clk_switch_src(1) != 0) {                     /* 1 = CPLL */
        return -1;
    }
    clk_mark("[clk] 6 已切源\r\n");
    ROM_ETS_UPDATE_CPU_FREQ(target_mhz);
    return 0;
}

/* 主入口：失败也能继续跑（退回 XTAL 40MHz），绝不因为提频把 bootloader 搞死 */
int s31_cpu_clk_boost(void)
{
    uint32_t before = s31_cpu_mhz();      /* mcycle × SYSTIMER 实测，权威口径 */
    uint32_t hw_before = s31_cpu_hw_mhz();
    int rc;

    rc = s31_cpu_clk_to_mhz(S31_CPU_TARGET_MHZ);

    /* 不管成功失败，都把 CherryUSB 看到的系统时钟同步成**实测值** */
    s31_usb_set_system_clock(s31_cpu_mhz() * 1000000u);

    /* ⚠️ 这里报的是 mcycle 实测值，**不是** CPU_SRC_FREQ0 的回读值：
       实测切到 320MHz 之后 `HP_SYS_CLKRST+0x168/0x16C` 仍读 75MHz / status=0x02
       （bit2 SRC_IS_CPLL 不置位）—— 说明参考工程那套回读的偏移或解释在 S31 上不准。
       mcycle 与 SYSTIMER 的比值在切频前后正好是 8 倍（= CPLL 的 40×8/1），可信。 */
    kprintf("[clk] 提频 %s: %u -> %u MHz（mcycle 实测；硬件回读 %u/%u 不可信）status=%08x\r\n",
            (rc == 0) ? "OK" : "失败",
            (unsigned)before, (unsigned)s31_cpu_mhz(),
            (unsigned)hw_before, (unsigned)s31_cpu_hw_mhz(),
            (unsigned)S31_REG32(R_CPU_CLK_STATUS0));
    return rc;
}
