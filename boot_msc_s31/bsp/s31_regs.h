/*===========================================================================
 * s31_regs.h -- ESP32-S31 裸机用到的寄存器地址与位定义
 *
 * 全部核对自 ESP-IDF 6.1：
 *   components/soc/esp32s31/ld/esp32s31.peripherals.ld      （外设基址）
 *   components/soc/esp32s31/register/soc/xxxx_struct.h     （寄存器偏移 / 位域）
 *   components/soc/esp32s31/include/soc/interrupts.h       （中断源号）
 *   components/esp_rom/esp32s31/ld/xxxx.ld                 （ROM 函数入口）
 * 每个数值后面的注释标了出处，方便回头核对。
 *===========================================================================*/
#ifndef S31_REGS_H
#define S31_REGS_H

#include <stdint.h>

#define S31_REG32(addr)      (*(volatile uint32_t *)(uintptr_t)(addr))
#define S31_REG8(addr)       (*(volatile uint8_t  *)(uintptr_t)(addr))
#define S31_SETBITS(a, m)    do { S31_REG32(a) |=  (uint32_t)(m); } while (0)
#define S31_CLRBITS(a, m)    do { S31_REG32(a) &= ~(uint32_t)(m); } while (0)

/* ---- 内存窗口（soc.h:152 / ld.hp_mem_defs）---- */
#define S31_RAM_LOW          0x2F000000u
#define S31_RAM_HIGH         0x2F07AFC0u   /* SRAM_SEG_END：二级 bootloader 保留区起点 */
/* bootloader 与 App 的内部 RAM 分界：App 的内部 RAM 段只允许落在 [此地址, S31_RAM_HIGH) */
#define S31_APP_IRAM_BASE    0x2F060000u

/* PSRAM 的虚拟地址窗口（soc.h:143 SOC_EXTRAM_LOW/HIGH）*/
#define S31_PSRAM_VADDR      0x50000000u
#define S31_PSRAM_VSIZE      0x04000000u   /* 映射窗口 64MB */

/* ---- 外设基址（esp32s31.peripherals.ld）---- */
#define S31_USB_OTGHS_BASE   0x20300000u   /* DWC2 HS 控制器 */
#define S31_CNNT_SYS_BASE    0x20359000u   /* USB 时钟/复位/PHY 走这里（IDF utmi_ll）*/
#define S31_USB_UTMI_BASE    0x20380000u   /* UTMI PHY 控制寄存器 */
#define S31_USJ_BASE         0x20391000u   /* USB-Serial/JTAG（板载 USB-DBG 口）*/
#define S31_SYSTIMER_BASE    0x20399000u
#define S31_SPIMEM0_BASE     0x20500000u
#define S31_SPIMEM1_BASE     0x20501000u
#define S31_SPIMEM2_BASE     0x20502000u   /* PSRAM 系统侧控制器（同级还有 SPIMEM3）*/
#define S31_SPIMEM3_BASE     0x20503000u   /* PSRAM 外设侧控制器（读写芯片 mode register 用）*/
#define S31_IO_MUX_BASE      0x20582000u
#define S31_GPIO_BASE        0x20583000u
#define S31_HP_SYS_CLKRST_BASE 0x20587000u
#define S31_HP_ALIVE_SYS_BASE  0x20589000u
#define S31_LP_SYS_BASE        0x20700000u
#define S31_LP_CLKRST_BASE     0x20701000u  /* LP_AONCLKRST：复位原因 / CPLL 分频 */
#define S31_PMU_BASE           0x20704000u
#define S31_MODEM_LPCON_BASE   0x2010f000u  /* MODEM_LPCON：+0x18 clk_conf.clk_i2c_mst_en(bit2) = 模拟 I2C 主机时钟 */
#define S31_INTR0_BASE         0x20585000u  /* 中断路由矩阵：+4*source ← CLIC ID */
#define S31_TIMG0_BASE         0x20580000u  /* timer group 0（MWDT 复位用）*/
#define S31_TIMG1_BASE         0x20581000u

/* TIMG MWDT（整片复位用；寄存器布局来自 rtt_nano_s31 的实测实现）
 *   WDTCONFIG0(+0x48)：bit31 wdt_en、bit[30:29] stg0 动作(3=RESET_SYSTEM)
 *   WDTCONFIG1(+0x4c)：stg0 计数、WDTCONFIG2(+0x50)：预分频
 *   WDTWPROTECT(+0x64)：key 0x50D83AA1
 * ⚠️ 别用 LP_AONCLKRST 的 core0 软复位（bit20）：那不是整片复位，
 *    是"把 core0 永久按在复位里"，只能物理上电恢复（rtt 工程踩过）。 */
#define S31_TIMG_WDTCONFIG0    0x48u
#define S31_TIMG_WDTCONFIG1    0x4cu
#define S31_TIMG_WDTCONFIG2    0x50u
#define S31_TIMG_WDTWPROTECT   0x64u
#define S31_WDT_WKEY           0x50D83AA1u

/*===========================================================================
 * USB OTG HS（DWC2 @0x20300000）
 *===========================================================================*/
/* --- 时钟 / 复位（IDF esp_hal_usb/esp32s31/include/hal/usb_utmi_ll.h）--- */
/* HP_SYS_CLKRST.usb_otghs_ctrl0 @+0xac：APB/SYS 时钟门控 */
#define S31_CLKRST_USB_OTGHS_CTRL0  (S31_HP_SYS_CLKRST_BASE + 0xacu)
#define S31_USB_CLK_APB_EN          (1u << 0)   /* reg_usb_otghs_apb_clk_en */
#define S31_USB_CLK_SYS_EN          (1u << 1)   /* reg_usb_otghs_sys_clk_en */

/* CNNT_SYS.sys_usb_otg20_ctrl @+0x30：PHY 参考时钟 / 三路复位
 *   🚨 偏移必须从 IDF 的 struct 头**逐字段累加**算出来 —— 注意 struct 里有
 *      `uint32_t reserved_008[2];` 这种**没有 volatile** 的字段，漏算就会整体偏 8 字节，
 *      把 USB 时钟写到隔壁寄存器上（真板实测踩过：控制器一直没时钟、GHWCFG 全 0）。*/
#define S31_CNNT_USB_CLK_CTRL       (S31_CNNT_SYS_BASE + 0x2cu)
#define S31_CNNT_USB_OTG20_CTRL     (S31_CNNT_SYS_BASE + 0x30u)
#define S31_USB20_UTMIFS_CLK_EN     (1u << 23)
#define S31_USB20_ULPI_CLK_EN       (1u << 24)
#define S31_USB20_PHYREF_SRC_SEL    (3u << 25)  /* [26:25] 0=12M 1=25M 2=pad */
#define S31_USB20_PHYREF_CLK_EN     (1u << 27)
#define S31_USB20_PHY_RST_EN        (1u << 29)
#define S31_USB20_AHB_RST_EN        (1u << 30)
#define S31_USB20_APB_RST_EN        (1u << 31)

/* CNNT_SYS.sys_hp_usb_device_ctrl @+0x34
 * 🚨 bit30 (sys_usb_device_48m_clk_en) 复位默认=1 —— 写这个寄存器**必须读-改-写**，
 *    整字写 0 会把 USB 设备块（CDC + JTAG）一起停振，只能物理断电恢复（rtt 工程踩过）。*/
#define S31_CNNT_USB_DEVICE_CTRL    (S31_CNNT_SYS_BASE + 0x34u)
#define S31_USB_DEVICE_48M_CLK_EN   (1u << 30)

/* HP_ALIVE_SYS.usb_ctrl @+0x24：D+/D- 15k 下拉（设备模式必须断开）*/
#define S31_ALIVE_USB_CTRL          (S31_HP_ALIVE_SYS_BASE + 0x24u)
#define S31_OTGHS_PHY_DMPULLDOWN    (1u << 2)
#define S31_OTGHS_PHY_DPPULLDOWN    (1u << 3)
#define S31_OTGHS_PHY_IDPULLUP      (1u << 4)

/* HP_ALIVE_SYS.usb_otghs_ctrl @+0xb0：PHY PLL / suspendm / reset 的手动控制 */
#define S31_ALIVE_USB_OTGHS_CTRL    (S31_HP_ALIVE_SYS_BASE + 0xb0u)
#define S31_OTGHS_PHY_PLL_FORCE_EN  (1u << 0)
#define S31_OTGHS_PHY_PLL_EN        (1u << 1)
#define S31_OTGHS_PHY_SUSPENDM_FORCE_EN (1u << 2)   /* 复位默认=1，必须清 0 交给 DWC2 */
#define S31_OTGHS_PHY_SUSPENDM      (1u << 3)
#define S31_OTGHS_PHY_OTG_SUSPENDM  (1u << 7)

/* LP_SYS.usb_ctrl @+0x100：挂起状态 / 唤醒清除 */
#define S31_LP_SYS_USB_CTRL         (S31_LP_SYS_BASE + 0x100u)
#define S31_LP_USB_WAKEUP_CLR       (1u << 2)
#define S31_LP_USB_IN_SUSPEND       (1u << 3)

/* USB_UTMI.fc_06 @+0x18：LS 模式支持 */
#define S31_UTMI_FC06               (S31_USB_UTMI_BASE + 0x18u)
#define S31_UTMI_FC06_LS_PAR_EN     (1u << 0)
#define S31_UTMI_FC06_LS_KPALV_EN   (1u << 3)

/* DWC2 控制器寄存器偏移（CherryUSB 的 dwc2 驱动自己会用，这里只用于自检打印）*/
#define S31_DWC2_GOTGCTL            0x000u
#define S31_DWC2_GSNPSID            0x040u
#define S31_DWC2_GHWCFG2            0x048u
#define S31_DWC2_GUSBCFG            0x00cu
#define S31_DWC2_GINTSTS            0x014u
#define S31_DWC2_DCTL               0x804u
#define S31_DWC2_DSTS               0x808u

/*===========================================================================
 * USB-Serial/JTAG（msh/打印控制台，板载 USB-DBG 口）
 *===========================================================================*/
#define S31_USJ_EP1          (S31_USJ_BASE + 0x00)  /* 写=发一个字节，读=收一个字节 */
#define S31_USJ_EP1_CONF     (S31_USJ_BASE + 0x04)
#define S31_USJ_INT_RAW      (S31_USJ_BASE + 0x08)
#define S31_USJ_INT_ST       (S31_USJ_BASE + 0x0c)
#define S31_USJ_INT_ENA      (S31_USJ_BASE + 0x10)
#define S31_USJ_INT_CLR      (S31_USJ_BASE + 0x14)
#define S31_USJ_CONF0        (S31_USJ_BASE + 0x18)
#define S31_USJ_EP1_WR_DONE          (1u << 0)
#define S31_USJ_IN_EP_DATA_FREE      (1u << 1)
#define S31_USJ_OUT_EP_DATA_AVAIL    (1u << 2)
#define S31_USJ_INT_RX               (1u << 2)   /* SERIAL_OUT_RECV_PKT_INT */
#define S31_USJ_INT_TX_EMPTY         (1u << 3)   /* SERIAL_IN_EMPTY_INT：TX FIFO 空 */
#define S31_USJ_CONF0_PAD_ENABLE     (1u << 14)

/*===========================================================================
 * CLIC（中断控制器）
 *===========================================================================*/
#define S31_CLIC_BASE        0x10800000u
#define S31_CLIC_CTRL_BASE   0x10801000u
#define S31_CLIC_IP(i)       (S31_CLIC_CTRL_BASE + (i) * 4 + 0)
#define S31_CLIC_IE(i)       (S31_CLIC_CTRL_BASE + (i) * 4 + 1)
#define S31_CLIC_ATTR(i)     (S31_CLIC_CTRL_BASE + (i) * 4 + 2)
#define S31_CLIC_CTL(i)      (S31_CLIC_CTRL_BASE + (i) * 4 + 3)
#define S31_CLIC_CTL_PRIO(p) ((uint8_t)((p) << 5))   /* 优先级 3 位，左对齐到 bit7:5 */
#define S31_CLIC_EXT_OFFSET  16                      /* 外部中断从 16 号起 */
/* 阈值：S31 是标准 CLIC → 用 mintthresh CSR(0x347)，level 0 ⇒ 0x1F */
#define S31_MINTTHRESH_ALL   0x1Fu

/* 中断源号（interrupts.h 的枚举，第一个 = 0）*/
#define S31_ETS_USB_SERIAL_JTAG     2
#define S31_ETS_SYSTIMER_TARGET0    33
#define S31_ETS_USB_OTGHS           99

/*===========================================================================
 * SYSTIMER（16MHz 固定时基：XTAL 40MHz / 2.5）
 *===========================================================================*/
#define S31_SYSTIMER_CONF           (S31_SYSTIMER_BASE + 0x00)
#define S31_SYSTIMER_UNIT0_OP       (S31_SYSTIMER_BASE + 0x04)
#define S31_SYSTIMER_UNIT0_VALUE_HI (S31_SYSTIMER_BASE + 0x40)
#define S31_SYSTIMER_UNIT0_VALUE_LO (S31_SYSTIMER_BASE + 0x44)
#define S31_SYSTIMER_UNIT0_LOAD_HI  (S31_SYSTIMER_BASE + 0x0c)
#define S31_SYSTIMER_UNIT0_LOAD_LO  (S31_SYSTIMER_BASE + 0x10)
#define S31_SYSTIMER_UNIT0_LOAD     (S31_SYSTIMER_BASE + 0x5c)
#define S31_SYSTIMER_CLK_EN         (1u << 31)
#define S31_SYSTIMER_UNIT0_WORK_EN  (1u << 30)
#define S31_SYSTIMER_UNIT0_OP_UPDATE (1u << 30)
#define S31_SYSTIMER_UNIT0_VALUE_VALID (1u << 29)
#define S31_SYSTIMER_HZ             16000000u
/* SYSTIMER 时钟门控在 HP_SYS_CLKRST（hp_sys_clkrst_reg.h）：bit0 APB / bit4 CLK */
#define S31_SYSTIMER_CTRL0          (S31_HP_SYS_CLKRST_BASE + 0x120u)
#define S31_SYSTIMER_APB_CLK_EN     (1u << 0)
#define S31_SYSTIMER_CLK_EN_BIT     (1u << 4)

/*===========================================================================
 * GPIO / IO_MUX
 *===========================================================================*/
#define S31_GPIO_STRAP_REG   (S31_GPIO_BASE + 0x00u)
#define S31_GPIO_OUT_W1TS    (S31_GPIO_BASE + 0x08u)
#define S31_GPIO_OUT_W1TC    (S31_GPIO_BASE + 0x0cu)
#define S31_GPIO_OUT1_W1TS   (S31_GPIO_BASE + 0x14u)
#define S31_GPIO_OUT1_W1TC   (S31_GPIO_BASE + 0x18u)
#define S31_GPIO_ENABLE_W1TS (S31_GPIO_BASE + 0x38u)
#define S31_GPIO_ENABLE_W1TC (S31_GPIO_BASE + 0x3cu)
#define S31_GPIO_ENABLE1_W1TS (S31_GPIO_BASE + 0x44u)
#define S31_GPIO_ENABLE1_W1TC (S31_GPIO_BASE + 0x48u)
#define S31_GPIO_IN_REG      (S31_GPIO_BASE + 0x64u)
#define S31_GPIO_IN1_REG     (S31_GPIO_BASE + 0x68u)
#define S31_GPIO_PIN_REG(n)  (S31_GPIO_BASE + 0xF4u + 4u * (n))
#define S31_GPIO_PIN_OD      (1u << 2)     /* PAD_DRIVER：1 = 开漏 */

#define S31_GPIO_PIN_COUNT   63
#define S31_GPIO_MASK(n)     (1u << ((n) & 31))
/* S31 的 IO_MUX 恰好按引脚号连续排列：offset = 4 * pin */
#define S31_IOMUX_REG(n)     (S31_IO_MUX_BASE + 4u * (n))
#define S31_IOMUX_MCU_SEL_S  12
#define S31_IOMUX_FUN_IE     (1u << 9)
#define S31_IOMUX_FUN_PU     (1u << 8)
#define S31_IOMUX_FUN_PD     (1u << 7)
#define S31_IOMUX_FUNC_GPIO  1u

/*===========================================================================
 * 复位原因（LP_AONCLKRST @0x20701000）
 *   bit0=flag、bit[6:1]=cause，写 bit30 清除。
 *   🚨 ROM 打印的 rst:0x.. 可能是上次留下的旧值 —— 我们开机先打印再清。
 *===========================================================================*/
#define S31_HPCORE0_RST_CAUSE  (S31_LP_CLKRST_BASE + 0x30u)
#define S31_HPCORE1_RST_CAUSE  (S31_LP_CLKRST_BASE + 0x34u)
#define S31_LPCORE_RST_CAUSE   (S31_LP_CLKRST_BASE + 0x38u)
#define S31_RST_CAUSE_CLR      (1u << 30)

#endif /* S31_REGS_H */
