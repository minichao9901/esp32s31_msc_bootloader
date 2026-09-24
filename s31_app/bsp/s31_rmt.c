/*===========================================================================
 * s31_rmt.c -- 裸机 RMT 发送（专给板载 WS2812 用）
 *
 * ★ 为什么用 RMT，而不是 CPU 软件翻转引脚
 *   WS2812 靠**高电平的长短**区分 0 和 1（T0H≈0.3µs / T1H≈0.9µs），
 *   整个位周期只有 1.2µs。软件翻转的精度完全取决于代码路径 —— 本工程在这一点上
 *   栽过两次：用 s31_delay_us() 时它的最小粒度 1µs 比整个位周期还长，
 *   350/700/900ns 三个延时全被抬成 1µs，芯片每次采样都读到高（每个 bit 都成了 1，
 *   灯常亮白色）；改成 mcycle 忙等后，GPIO 读改写的开销又全叠进位周期（慢 1.9 倍）。
 *   → 时序这种事交给硬件：CPU 只填符号表，RMT 按 tick 精确走完，不受中断/取指/cache 影响。
 *
 * ★ 三条必须照做的（每一条都对应过一次"灯不对、但没有任何报错"）
 *   ① **符号写进 RMTMEM，不是 chndata**（IDF 的做法：rmt_hal.c:15 + rmt_tx.c:352）
 *        rmt_ll_enable_mem_access_nonfifo(dev, true)   -> sys_conf.apb_fifo_mask = 1
 *        内存窗口 RMTMEM = 0x20355800（见 soc/esp32s31/ld/esp32s31.peripherals.ld；
 *        布局 channels[4].symbols[48]，见 IDF rmt_private.h 的 rmt_block_mem_t），
 *        通道 ch 的基址 = RMTMEM + ch × SOC_RMT_MEM_WORDS_PER_CHANNEL。
 *        ⚠️ 往 chndata 写（apb_fifo_mask 复位默认 0 = FIFO 那条路）**发不出数据**：
 *           TX 引擎一启动就没符号可发，现象是"一帧只花几微秒就发完"、灯不亮 ——
 *           极易误判成时基或 CPU 主频有问题（本人就误判了一轮）。
 *        另需 rmt_ll_mem_force_power_on() 和 sys_conf.mem_clk_force_on = 1。
 *   ② 符号用 IDF 的 `rmt_symbol_word_t` 按**字段名**填，不手拼位域
 *      （本项目为"手抄位号"付过两次学费，见 PSRAM.md §2 §12）。
 *   ③ 等 TX_DONE **必须带超时**：RMT 没配上时这个标志永远不来，无上限死等会挂住整机。
 *
 * ★ 位时序（照抄 IDF 例程 projects/led_rgb/main/led_strip_encoder.c）
 *      T0H=0.3µs T0L=0.9µs   T1H=0.9µs T1L=0.3µs   reset=50µs
 *   时基 = XTAL 40MHz ÷1(组) ÷4(通道) = 10MHz，1 tick = 0.1µs。
 *   ⚠️ 不选 IDF 默认的 PLL_F80M：那是另一个 PLL，裸机里没人替我们打开；
 *      XTAL 一定在跑（进 bootloader 时 CPU 就跑在它上面）。
 *   ⚠️ RMT 的位时序**与 CPU 主频无关**（源在 HP_SYS_CLKRST.rmt_ctrl0 里选）。
 *
 * ★ 自证：send() 写完会把 25 个符号**回读比对**，不一致返回 -2。
 *   "符号到底进没进内存"没法从灯的颜色看出来，只能读回来 —— 这条检查就是①的护栏。
 *===========================================================================*/
#include <stdint.h>

#include "s31_regs.h"
#include "soc/reg_base.h"           /* DR_REG_RMT_BASE */
#include "soc/rmt_struct.h"         /* rmt_dev_t */
#include "soc/soc_caps.h"           /* SOC_RMT_MEM_WORDS_PER_CHANNEL */
#include "soc/clk_tree_defs.h"      /* RMT_CLK_SRC_XTAL */
#include "soc/gpio_sig_map.h"       /* RMT_SIG_OUT0_IDX */

/* ★ IDF 把外设实例定义在 `components/soc/esp32s31/` 下的 `xxx_periph.c` 里，
 *   而裸机工程链不到那些 .c —— 所以这里**用宏把符号顶掉**：直接按地址解引用。
 *
 *   ⚠️ 定义位置有讲究：必须在对应 `*_struct.h` **之后**。
 *      那些头里有 `extern hp_system_dev_t HP_SYSTEM;` 之类的声明，
 *      宏要是在它前面定义，那句声明会被展开成 `extern ... (*(...))` —— 语法错误。
 *      放后面则靠 include guard 让 rmt_ll.h 里那次 include 变成空操作。
 *   ⚠️ 但宏体里用到的地址常量必须**在宏被展开之前**就定义好 ——
 *      `rmt_ll.h` 里那些 static inline 会立刻展开 HP_SYSTEM。 */
#define S31_HP_SYSTEM_BASE  0x20586000u     /* HP_SYSTEM （esp32s31.peripherals.ld）*/
#define S31_RMTMEM_BASE     0x20355800u     /* RMTMEM    （同上）*/

#include "soc/hp_system_struct.h"    /* HP_SYSTEM.sys_rmt_mem_lp_ctrl（内存掉电控制）*/
#include "soc/hp_sys_clkrst_struct.h"
#define HP_SYSTEM     (*(hp_system_dev_t *)S31_HP_SYSTEM_BASE)
#define HP_SYS_CLKRST (*(hp_sys_clkrst_dev_t *)S31_HP_SYS_CLKRST_BASE)

#include "hal/rmt_types.h"          /* rmt_symbol_word_t */
#include "hal/rmt_ll.h"             /* rmt_ll_*（按字段名写）*/

int      kprintf(const char *fmt, ...);
uint64_t s31_systimer_ticks(void);

#define LED_GPIO        60          /* 板载 WS2812 数据脚（J2 第 6 脚也是它）*/

#define RMT_TICK_HZ     10000000u   /* 1 tick = 0.1µs */
#define RMT_SRC_HZ      40000000u   /* 源 = XTAL 40MHz */
#define RMT_CH_DIV      (RMT_SRC_HZ / RMT_TICK_HZ)      /* 4 */

#define RMT_CH          0
#define RMT_FRAME_SYMS  25          /* 24 个数据位 + 1 个复位符号 */
#define WS_RESET_TICKS  500         /* 50µs @10MHz */

/* RMT 通道内存窗口（不在 rmt_dev_t 里，是独立的一段）
 *   布局对齐 IDF `rmt_private.h` 的 rmt_block_mem_t：channels[4].symbols[48] */
typedef struct {
    rmt_symbol_word_t symbols[SOC_RMT_MEM_WORDS_PER_CHANNEL];
} s31_rmt_chan_mem_t;

#define RMT_CH_MEM(ch)  (((volatile s31_rmt_chan_mem_t *)S31_RMTMEM_BASE)[(ch)].symbols)

/* 按**字段名**填一个符号：level0/duration0 是前半段，level1/duration1 是后半段 */
static inline void rmt_sym(volatile rmt_symbol_word_t *s,
                           uint16_t d0, uint8_t l0, uint16_t d1, uint8_t l1)
{
    s->duration0 = d0;
    s->level0    = l0;
    s->duration1 = d1;
    s->level1    = l1;
}

static rmt_dev_t *rmt(void)
{
    return (rmt_dev_t *)DR_REG_RMT_BASE;
}

/* 一次性配置：模块时钟 + 通道 0 内存 + 把 GPIO60 接到 RMT 的输出信号上 */
void s31_rmt_init(void)
{
    rmt_dev_t *d = rmt();
    uint32_t v;

    /* ---- 模块时钟（顺序照 IDF rmt_common.c:44-45,161-162）---- */
    rmt_ll_enable_bus_clock(0, true);       /* HP_SYS_CLKRST.rmt_ctrl0.reg_rmt_sys_clk_en */
    rmt_ll_reset_register(0);
    rmt_ll_set_group_clock_src(d, RMT_CH, RMT_CLK_SRC_XTAL, 1, 0, 0);
    rmt_ll_enable_group_clock(d, true);     /* reg_rmt_clk_en */

    /* ---- 内存：上电 + 直连（★ 见文件头「三条必须照做的」①）---- */
    rmt_ll_mem_force_power_on(d);           /* HP_SYSTEM.sys_rmt_mem_lp_ctrl：强制上电 */
    d->sys_conf.mem_clk_force_on = 1;       /* 内存时钟强制打开（复位默认 0）*/
    rmt_ll_enable_mem_access_nonfifo(d, true);   /* apb_fifo_mask=1：APB 直连内存 */

    /* ---- 通道 0 ---- */
    rmt_ll_tx_set_channel_clock_div(d, RMT_CH, RMT_CH_DIV);     /* 40MHz ÷ 4 = 10MHz */
    rmt_ll_tx_set_mem_blocks(d, RMT_CH, 1);                     /* 1 块 = 48 个字 */
    rmt_ll_tx_set_limit(d, RMT_CH, SOC_RMT_MEM_WORDS_PER_CHANNEL);  /* 中断阈值（只等 TX_DONE）*/
    rmt_ll_tx_enable_loop(d, RMT_CH, false);
    rmt_ll_tx_fix_idle_level(d, RMT_CH, 0, true);               /* 空闲时输出**低** */
    rmt_ll_tx_reset_pointer(d, RMT_CH);
    rmt_ll_clear_interrupt_status(d, 0xFFFFFFFFu);

    /* ---- GPIO60 -> RMT_SIG_OUT0 ---- */
    /* ① 焊盘功能选 GPIO（而不是某个外设的直连）*/
    v = S31_REG32(S31_IOMUX_REG(LED_GPIO));
    v &= ~((0x7u << S31_IOMUX_MCU_SEL_S) | S31_IOMUX_FUN_IE);
    v |= (S31_IOMUX_FUNC_GPIO << S31_IOMUX_MCU_SEL_S);
    S31_REG32(S31_IOMUX_REG(LED_GPIO)) = v;

    /* ② 输出使能交给普通 GPIO（oen_sel=0）：这样**焊盘一直被驱动**，
          电平由下面 out_sel 选的 RMT 信号决定。
          让 oen_sel=1（由外设的 oen 控制）也行，但那要赌 RMT 的 oen 行为，
          这里选确定的那条路。 */
    S31_SETBITS(S31_GPIO_ENABLE1_W1TS, S31_GPIO_MASK(LED_GPIO));
    S31_CLRBITS(S31_GPIO_OUT1_W1TC, S31_GPIO_MASK(LED_GPIO));   /* 先给低 */

    /* ③ 输出信号选 RMT_SIG_OUT0（只有低 9 位是 out_sel，其余位保持 0）*/
    S31_REG32(S31_GPIO_FUNC_OUT_SEL(LED_GPIO)) = (RMT_SIG_OUT0_IDX & 0x1FFu);
}

/* 发一帧 WS2812：grb[3] = {G, R, B}（WS2812 的字节序就是 GRB）。
 * 阻塞到硬件发完（25 个符号 ≈ 24×1.2µs + 50µs ≈ 78.8µs）。
 *   0 = 正常   -1 = 等 TX_DONE 超时（RMT 没跑起来）   -2 = 符号没写进通道内存 */
int s31_rmt_ws2812_send(const uint8_t *grb)
{
    rmt_dev_t *d = rmt();
    volatile rmt_symbol_word_t *mem = RMT_CH_MEM(RMT_CH);
    rmt_symbol_word_t frame[RMT_FRAME_SYMS];
    uint64_t t0;
    int i, b, n = 0;

    rmt_ll_clear_interrupt_status(d, RMT_LL_EVENT_TX_DONE(RMT_CH));
    rmt_ll_tx_reset_pointer(d, RMT_CH);

    /* ① 组帧：WS2812 是 MSB 先出 */
    for (i = 0; i < 3; i++) {
        for (b = 7; b >= 0; b--) {
            if ((grb[i] >> b) & 1u) {
                rmt_sym(&frame[n], 9, 1, 3, 0);     /* 1：高 0.9µs + 低 0.3µs */
            } else {
                rmt_sym(&frame[n], 3, 1, 9, 0);     /* 0：高 0.3µs + 低 0.9µs */
            }
            n++;
        }
    }
    rmt_sym(&frame[n], WS_RESET_TICKS, 0, 0, 0);    /* 帧间 50µs 低电平复位 */
    n++;

    /* ② 写进通道内存 + 回读自证（写不进去就只能返回错，灯是不会报错的）*/
    for (i = 0; i < n; i++) {
        mem[i] = frame[i];
    }
    for (i = 0; i < n; i++) {
        if (mem[i].val != frame[i].val) {
            return -2;
        }
    }

    /* ③ 启动，等 TX_DONE（带超时：RMT 没配上时这个标志永远不来）*/
    rmt_ll_tx_start(d, RMT_CH);

    t0 = s31_systimer_ticks();
    while (!(d->int_raw.val & RMT_LL_EVENT_TX_DONE(RMT_CH))) {
        if (s31_systimer_ticks() - t0 > (S31_SYSTIMER_HZ / 200u)) {   /* > 5ms */
            return -1;
        }
    }
    rmt_ll_clear_interrupt_status(d, RMT_LL_EVENT_TX_DONE(RMT_CH));
    return 0;
}
