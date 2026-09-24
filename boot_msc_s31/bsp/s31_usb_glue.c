/*===========================================================================
 * s31_usb_glue.c -- CherryUSB <-> ESP32-S31 裸机对接层
 *
 * 替代 IDF 版的 port/dwc2/usb_glue_esp.c，提供 DWC2 驱动需要的：
 *   usb_dc_low_level_init()      时钟/复位/PHY
 *   dwc2_get_user_params()       FIFO 划分（取自 IDF 的 S31 参数）
 *   usbd_dwc2_delay_ms()
 *   usbd_dwc2_get_system_clock() （CherryUSB 用它算 turnaround）
 *
 * 寄存器动作逐条对照 IDF：
 *   esp_hal_usb/esp32s31/include/hal/usb_utmi_ll.h
 *     _usb_utmi_ll_enable_bus_clock() / _usb_utmi_ll_reset_register()
 *     usb_utmi_ll_enable_data_pulldowns() / enable_precise_detection() / configure_ls()
 *===========================================================================*/
#include <stdint.h>
#include <string.h>

int kprintf(const char *fmt, ...);
#include "usbd_core.h"
#include "usb_dwc2_param.h"
#include "s31_regs.h"

void s31_delay_ms(uint32_t ms);
void s31_delay_us(uint32_t us);

/*===========================================================================
 * FIFO 划分（来自 IDF 的 S31 param_hs；总 896 words = 3584 字节）
 *   ⚠️ 故意用 **FIFO（从）模式** device_dma_enable=false：
 *      DWC2 不直接访问内存 → 没有 cache 一致性问题，裸机首选。
 *      （P4 那份裸机工程也这么选，且实测 12MB/s。）
 *===========================================================================*/
static const struct dwc2_user_params s31_param_hs = {
    .phy_type = DWC2_PHY_TYPE_PARAM_UTMI,
    .device_dma_enable = false,
    .device_dma_desc_enable = false,
    .device_rx_fifo_size = (896 - 16 - 128 - 128 - 128 - 128 - 16 - 16),   /* 336 words */
    .device_tx_fifo_size = {
        [0] = 16,  /* 64 byte  */
        [1] = 128, /* 512 byte */
        [2] = 128, /* 512 byte */
        [3] = 128, /* 512 byte */
        [4] = 128, /* 512 byte */
        [5] = 16,  /* 64 byte  */
        [6] = 16,  /* 64 byte  */
        [7] = 0, [8] = 0, [9] = 0, [10] = 0,
        [11] = 0, [12] = 0, [13] = 0, [14] = 0, [15] = 0 },
    .host_dma_desc_enable = false,
    .host_rx_fifo_size = (896 - 128 - 128),
    .host_nperio_tx_fifo_size = 128,
    .host_perio_tx_fifo_size = 128,
};

/* CherryUSB 用它算 turnaround time。S31 的 bootROM 交棒时 CPU 跑 XTAL 40MHz
   （rtt 工程实测），后面若提频要同步这个值。 */
uint32_t SystemCoreClock = 40000000u;

void s31_usb_set_system_clock(uint32_t hz)
{
    SystemCoreClock = hz;
}

/*===========================================================================
 * 硬件初始化
 *===========================================================================*/
static void usb_hw_init(void)
{
    /* 1) 总线时钟：HP_SYS_CLKRST.usb_otghs_ctrl0 @0x205870ac（APB + SYS）*/
    S31_SETBITS(S31_CLKRST_USB_OTGHS_CTRL0, S31_USB_CLK_APB_EN | S31_USB_CLK_SYS_EN);

    /* 2) 复位脉冲。🚨 实测关键点：CNNT_SYS.sys_usb_otg20_ctrl 的三路复位
     *    **复位默认是"按住"的**（默认值读回来 0xE0800000 = phy/ahb/apb rst 全 1），
     *    而且在复位按住期间**对这个寄存器的写会被吃掉、读回 0**。
     *    所以"先开时钟再放复位"的顺序会把时钟位一起写没 —— 真板踩过：
     *    控制器一直没时钟（GSNPSID/GHWCFG 全 0），CherryUSB 报
     *    "device_rx_fifo_size cannot be larger than power_on_value 0" 然后死等。
     *    正确顺序：先放复位（先 PHY 后控制器），**再**开时钟。
     *    这个寄存器只能读-改-写：整字写会清掉别的位（rtt 工程为此烧过一次板）。 */
    S31_SETBITS(S31_CNNT_USB_OTG20_CTRL,
                S31_USB20_PHY_RST_EN | S31_USB20_AHB_RST_EN | S31_USB20_APB_RST_EN);
    s31_delay_us(10);
    S31_CLRBITS(S31_CNNT_USB_OTG20_CTRL, S31_USB20_PHY_RST_EN);
    s31_delay_us(10);
    S31_CLRBITS(S31_CNNT_USB_OTG20_CTRL, S31_USB20_AHB_RST_EN | S31_USB20_APB_RST_EN);
    s31_delay_us(10);

    /* 3) 复位放掉之后，才开 UTMI / PHY 参考时钟 */
    S31_SETBITS(S31_CNNT_USB_OTG20_CTRL,
                S31_USB20_UTMIFS_CLK_EN | S31_USB20_PHYREF_CLK_EN);

    /* 4) 交给 DWC2 自动控制 PHY 的 suspend/PLL（清掉软件强制位）
     *    IDF 注释：reg_usb_otghs_phy_suspendm_force_en 在 S31 上复位默认=1，
     *    必须清 0，否则端口的自动 suspend/resume 不工作。 */
    S31_CLRBITS(S31_ALIVE_USB_OTGHS_CTRL,
                S31_OTGHS_PHY_SUSPENDM_FORCE_EN | S31_OTGHS_PHY_PLL_FORCE_EN);

    /* 5) 设备模式：断开 D+/D- 的 15k 下拉 */
    S31_CLRBITS(S31_ALIVE_USB_CTRL,
                S31_OTGHS_PHY_DPPULLDOWN | S31_OTGHS_PHY_DMPULLDOWN);

    /* 6) UTMI：允许 LS 模式 */
    S31_SETBITS(S31_UTMI_FC06, S31_UTMI_FC06_LS_PAR_EN | S31_UTMI_FC06_LS_KPALV_EN);

    /* 7) 精确的 VBUS/断开检测 */
    S31_SETBITS(S31_ALIVE_USB_OTGHS_CTRL, S31_OTGHS_PHY_OTG_SUSPENDM);

    /* 8) 清一次挂起/唤醒状态 */
    S31_SETBITS(S31_LP_SYS_USB_CTRL, S31_LP_USB_WAKEUP_CLR);
}

/*===========================================================================
 * 把 USB PHY 干净地关掉（= usb_hw_init 的逆操作）
 *
 * 🚨 为什么必须做：`S31_ALIVE_USB_OTGHS_CTRL` / `S31_LP_SYS_USB_CTRL` 这些都在
 * **常电域（HP_ALIVE / LP_SYS）**，**跨复位保留**。所以：
 *   1. bootloader 模式那次把 PHY 上电、D+ 上拉使能；
 *   2. 用户拔掉模式键复位 -> bootloader 探测到 App 合法 -> **直接跳进 App**，
 *      这条路径上**根本不会调 usbd_initialize()**，也就没人去关 PHY；
 *   3. 主机于是看到一个**永远不回答控制传输的僵尸设备**（App 不碰 USB），
 *      几秒后判定「设备描述符无效」并把这个失败状态**缓存住**；
 *   4. 之后无论怎么复位，主机都**从没见过一次干净的"拔出"**，就一直判失败，
 *      只有长时间断开（重新烧录 / 拔线）才恢复。
 * 实测现象完全吻合：纯 bootloader 模式反复复位 4/4 正常；
 * 而"跑过 App 之后再回来"就识别不了、且烧一次就好。
 *
 * 做法：撤 D+ 上拉 + 关 PHY 时钟/电源 + 断 15k 下拉，并且**保持住**（不恢复），
 * 这样 App 期间主机看到的是"设备已拔出"这一干净状态。
 *===========================================================================*/
#define S31_DWC2_DCTL_SDIS  (1u << 1)   /* Soft Disconnect */

void s31_usb_phy_off(void)
{
    if (S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GSNPSID) != 0u) {
        /* DWC2 有电时先软断开（撤掉 D+ 上拉），再断 PHY */
        S31_SETBITS(S31_USB_OTGHS_BASE + S31_DWC2_DCTL, S31_DWC2_DCTL_SDIS);
        s31_delay_ms(200);              /* 让主机确实处理到"已拔出" */
    }

    /* 关 PHY 时钟与 PHYREF（复位放掉的反操作）*/
    S31_CLRBITS(S31_CNNT_USB_OTG20_CTRL, S31_USB20_UTMIFS_CLK_EN | S31_USB20_PHYREF_CLK_EN);

    /* 软件强制 PHY 进 suspend（PHY 停止驱动总线）*/
    S31_SETBITS(S31_ALIVE_USB_OTGHS_CTRL,
                S31_OTGHS_PHY_SUSPENDM_FORCE_EN | S31_OTGHS_PHY_PLL_FORCE_EN);

    /* 总线时钟也关掉 */
    S31_CLRBITS(S31_CLKRST_USB_OTGHS_CTRL0, S31_USB_CLK_APB_EN | S31_USB_CLK_SYS_EN);
}

/* 把 USB 通路上所有关键寄存器摊开（一次启动就能定位断在哪一环）*/
void s31_usb_dump(const char *tag)
{
    kprintf("[usb:%s] clkrst=%08x cnnt_otg20=%08x cnnt_dev=%08x alive_ctrl=%08x alive_otg=%08x utmi_fc06=%08x\r\n",
            tag,
            (unsigned)S31_REG32(S31_CLKRST_USB_OTGHS_CTRL0),
            (unsigned)S31_REG32(S31_CNNT_USB_OTG20_CTRL),
            (unsigned)S31_REG32(S31_CNNT_USB_DEVICE_CTRL),
            (unsigned)S31_REG32(S31_ALIVE_USB_CTRL),
            (unsigned)S31_REG32(S31_ALIVE_USB_OTGHS_CTRL),
            (unsigned)S31_REG32(S31_UTMI_FC06));
    kprintf("[usb:%s] DWC2 GSNPSID=%08x GHWCFG2=%08x DSTS=%08x DCTL=%08x\r\n",
            tag,
            (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GSNPSID),
            (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GHWCFG2),
            (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DSTS),
            (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DCTL));
}

/*===========================================================================
 * USB 软断开 → 重连
 *
 * 对主机来说这和"把 USB-HS 线拔下来再插上"是**完全等价**的（就是撤掉 D+ 上拉再恢复）。
 *
 * 为什么需要它：主机（Windows）在**描述符读取失败之后，会把那个端口的失败状态缓存住、
 * 不再重试**。现场特征非常好认：
 *   设备管理器里一个「未知 USB 设备(设备描述符无效)」= VID_0000&PID_0005，
 *   而固件这边早已打出 [usb] ready、寄存器自检也和正常时逐位相同、cfg 永远是 0。
 * → 固件本身没坏，是主机侧卡住了。有了这个函数，固件自己就能把枚举救回来，
 *   不用人去拔线（板子跑在别人手里 / 远程调试时尤其有用）。
 *===========================================================================*/
/* S31_DWC2_DCTL_SDIS 已在文件上方定义（s31_usb_phy_off 用）*/

/* 🚨 断开必须**持续足够久**。第一版只断了 60 ms —— 完全没用：
   实测"能救回来"的那次是**烧录**（esptool 进下载模式，USB 设备真的消失了好几秒），
   主机要看到足够长的"设备已拔出"才会把"描述符读取失败"的缓存清掉、重新枚举。
   60 ms 对 Windows 来说太短，它根本来不及处理 detach 事件。
   所以这里断开 1.5 秒 —— 对主机等价于"拔下来停一下再插上"。 */
#define S31_USB_RECONNECT_OFF_MS  1500u

void s31_usb_soft_reconnect(void)
{
    uint32_t dctl = S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DCTL);

    S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DCTL) = dctl | S31_DWC2_DCTL_SDIS;   /* 断开（撤 D+ 上拉）*/
    s31_delay_ms(S31_USB_RECONNECT_OFF_MS);                                      /* 让主机确实看到"已拔出" */
    S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DCTL) = dctl & ~S31_DWC2_DCTL_SDIS;  /* 重连 */
    s31_delay_ms(100);
}

/*===========================================================================
 * CLIC 中断：SoC 源 99 (ETS_USB_OTGHS) → CLIC ID 16
 *   ⚠️ 必须在 usbd_initialize() 成功之后再使能（顺序反了会以 NULL handler 进中断）
 *===========================================================================*/
#define S31_USB_CLIC_ID  16

void USBD_IRQHandler(uint8_t busid);
void s31_irq_install(int id, void (*fn)(int id, void *arg), void *arg);

/* 诊断计数：用来分清"主机根本没跟我们说话" vs "说了但我们没答对"。
   实测价值极高 —— 见 README「无法识别的设备」那一节。 */
volatile uint32_t g_usb_irq_cnt;        /* USB ISR 进过几次（0 = 主机压根没打扰我们）*/
volatile uint32_t g_usb_gintsts_last;   /* ISR 里读到的中断状态（看是哪一类事件）*/
/* 🚨 "现在是不是在 USB 中断里"。CherryUSB 的日志（USB_LOG_ERR）有几处就在中断路径里
   （usbd_core.c:911/918/925 的 usbd_print_setup 会把整个 SETUP 包 dump 出来）。
   中断里碰控制台会形成正反馈：ISR 变慢 -> 主机超时 -> 更多错误 -> 打印更多；
   而且 kprintf 的环形缓冲不可重入，和主循环的日志撞上会丢 head 更新。
   所以 usb_config.h 里的 CONFIG_USB_PRINTF 用它当闸门：中断上下文直接丢弃。 */
volatile int g_usb_in_isr;

static void s31_usb_isr(int id, void *arg)
{
    (void)id;
    (void)arg;
    g_usb_irq_cnt++;
    g_usb_gintsts_last = S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GINTSTS);
    g_usb_in_isr = 1;
    USBD_IRQHandler(0);
    g_usb_in_isr = 0;
}

/* 主循环里轮询用的两个取数口（GINTSTS 是"当前是否还有挂起中断"，
   如果它一直有 bit 但 g_usb_irq_cnt 不涨 → 中断根本没送到 CPU = CLIC 路由问题）*/
uint32_t s31_usb_gintsts(void) { return S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GINTSTS); }
uint32_t s31_usb_dsts(void)    { return S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DSTS); }

void s31_usb_interrupt_enable(void)
{
    /* 机器模式中断阈值 = 0（放行所有优先级），标准 CLIC 用 mintthresh CSR */
    __asm__ volatile ("csrw 0x347, %0" : : "r"((uint32_t)S31_MINTTHRESH_ALL));

    /* 路由矩阵：源 99 → CLIC ID 16 */
    S31_REG32(S31_INTR0_BASE + 4u * S31_ETS_USB_OTGHS) = S31_USB_CLIC_ID;

    /* CLIC 线：非向量 + 电平触发 + 优先级 1 + 机器模式 */
    S31_REG8(S31_CLIC_IP(S31_USB_CLIC_ID))   = 0;
    S31_REG8(S31_CLIC_ATTR(S31_USB_CLIC_ID)) = 0;
    S31_REG8(S31_CLIC_CTL(S31_USB_CLIC_ID))  = S31_CLIC_CTL_PRIO(1);
    s31_irq_install(S31_USB_CLIC_ID, s31_usb_isr, 0);
    S31_REG8(S31_CLIC_IE(S31_USB_CLIC_ID))   = 1;

    /* 打开这一路的外部中断位（CLIC 外部线从 16 号起 → mie bit16）*/
    __asm__ volatile ("csrs mie, %0" : : "r"(1UL << S31_USB_CLIC_ID));
    __asm__ volatile ("csrs mstatus, 8");          /* MIE = 1 */
}

/*===========================================================================
 * CherryUSB 需要的接口
 *===========================================================================*/
void usb_dc_low_level_init(uint8_t busid)
{
    (void)busid;
    s31_usb_dump("before");
    usb_hw_init();
    s31_usb_dump("after");
}

void usb_dc_low_level_deinit(uint8_t busid)
{
    (void)busid;
}

void dwc2_get_user_params(uint32_t reg_base, struct dwc2_user_params *params)
{
    (void)reg_base;
    memcpy(params, &s31_param_hs, sizeof(struct dwc2_user_params));
}

void usbd_dwc2_delay_ms(uint8_t ms)
{
    s31_delay_ms((uint32_t)ms);
}

uint32_t usbd_dwc2_get_system_clock(void)
{
    return SystemCoreClock;
}
