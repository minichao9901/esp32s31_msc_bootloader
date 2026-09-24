/*
 * usb_config.h -- CherryUSB 裸机配置（ESP32-S31）
 *
 * 裸机（无 ESP-IDF、无 FreeRTOS）：
 *   - printf 走 kprintf（USB-Serial/JTAG）
 *   - 不开任何 osal 线程：EP0 在中断里处理，MSC 回调直接做
 *   - 端口 = ESP32-S31 USB-OTG HS（DWC2.0 @0x20300000，UTMI PHY）
 */
#ifndef USB_CONFIG_H
#define USB_CONFIG_H

#include <stdint.h>

/* ================ USB common Configuration ================ */
int kprintf(const char *fmt, ...);
extern volatile int g_usb_in_isr;   /* 由 s31_usb_glue.c 的 USB ISR 维护 */

/* 🚨 库日志必须加"中断闸门"。
   CherryUSB 的 USB_LOG_ERR 在**任何**等级下都是打开的（ERROR=0 最低），
   而它有几处调用点就在 **USB 中断**里 —— 最狠的是 usbd_core.c:911/918/925 的
   usbd_print_setup()，会把**整个 SETUP 包** dump 出来（约 70 字节）。
   在中断里打印的害处有两层：
     ① 正反馈：ISR 变慢 -> 主机控制传输超时("设备描述符无效") -> 报更多错 -> 打印更多；
     ② kprintf 的发送环形缓冲**不可重入**：主循环正在打状态行时被中断插进来，
        head 指针的更新会丢 -> 字节被覆盖 / 环形缓冲卡死（这也是日志出现乱码的来源之一）。
   所以：**中断上下文直接丢弃**，非中断（初始化那批 [I/USB] 参数 dump）照常输出。 */
#define CONFIG_USB_PRINTF(...)                                  \
    do {                                                        \
        if (!g_usb_in_isr) {                                    \
            kprintf(__VA_ARGS__);                               \
        }                                                       \
    } while (0)

/* USB_DBG_INFO：平时用这个。别长时间开 USB_DBG_LOG —— 它会往控制台狂打日志，
   而我们的发送环形缓冲被填满就丢字符。 */
#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

/* 数据对齐（DMA/cache 用；我们走 FIFO 模式，保持 32 即可） */
#ifndef CONFIG_USB_ALIGN_SIZE
#define CONFIG_USB_ALIGN_SIZE 32
#endif

/* 不走 dcache 维护：FIFO（从）模式下 DWC2 完全不碰内存，
   usb_dcache_* 在没定义 CONFIG_USB_DCACHE_ENABLE 时是空宏。 */
#define USB_NOCACHE_RAM_SECTION

/* ================= USB Device Stack Configuration ================ */
/* EP0 收发缓冲 */
#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

/* ⚠️ 不要定义 CONFIG_USBDEV_EP0_THREAD：裸机没线程，EP0 在 ISR 里处理 */
/* ⚠️ 不要定义 CONFIG_USBDEV_MSC_THREAD / MSC_POLLING：同上 */

#define CONFIG_USBDEV_MAX_BUS 1

/* ================= MSC class ================ */
#ifndef CONFIG_USBDEV_MSC_MAX_LUN
#define CONFIG_USBDEV_MSC_MAX_LUN 1
#endif
/* 一次 SCSI 传输的最大数据量（类里的 bounce buffer 大小）。
   磁盘块 512B，Windows 的 WRITE(10) 常见 64KB 一包 —— 设大一点能少几次回调，
   但缓冲占 RAM，64KB 够用。 */
#ifndef CONFIG_USBDEV_MSC_MAX_BUFSIZE
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE 65536
#endif

/* SCSI INQUIRY 里报的厂商/产品/版本（探索器里"设备属性"能看到）*/
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING "Espressif"
#define CONFIG_USBDEV_MSC_PRODUCT_STRING      "S31 MSC Bootloader"
#define CONFIG_USBDEV_MSC_VERSION_STRING      "1.00"

/* ================= USB DEVICE Port Configuration ================ */
#define CONFIG_USB_DWC2_PORT   HS_PORT
#define CONFIG_USB_HS

#define ESP_USB_HS0_BASE  0x20300000UL   /* USB-OTG HS (DWC2.0) */
#define USB_BASE          ESP_USB_HS0_BASE

/* ================= Interface to actual registers ================ */
/* HS 用 UTMI PHY；设备模式下 15k 下拉必须断开（见 s31_usb_glue.c）*/

#endif /* USB_CONFIG_H */
