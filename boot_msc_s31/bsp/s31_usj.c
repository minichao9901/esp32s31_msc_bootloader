/*===========================================================================
 * s31_usj.c -- 控制台：USB-Serial/JTAG（板载 USB-DBG 口，本机 COM43）
 *
 * 为什么不用 UART0：本板 UART0（GPIO58/59）没接线，USB-DBG 是唯一接着 PC 的口，
 * 而且 ROM 已经把这个外设配好了，我们只跟两个寄存器打交道：
 *   EP1      (+0x00)  写 = 往主机发一个字节；读 = 收主机来的一个字节
 *   EP1_CONF (+0x04)  bit0 wr_done（把整包交给主机）
 *                     bit1 serial_in_ep_data_free（TX FIFO 有空位）
 *
 * 【为什么要发送环形缓冲】TX FIFO 只有 64 字节，而"打开串口会让芯片复位"
 * （USB-JTAG 的 DTR/RTS 就是复位线）。复位后固件开机打印时主机还没来得及读，
 * 直接写 FIFO 会把横幅开头啃掉。缓冲之后 2KB 以内的开机输出一条不丢。
 *
 * 【wr_done 必须每次都写】哪怕一个字节都没写：
 *   IDF usb_serial_jtag_ll.h 写明"装满 64 字节的 FIFO 会被硬件自动提交，而主机把
 *   这个整包当成**未结束的 USB 事务**，要再补一个零长度包（再写一次 wr_done）
 *   才会真正交给 CDC 读端"。rtt 工程把它"优化"成只在写了字节后置，
 *   结果是串口一个字节都收不到 —— 这是那边最贵的一次自作聪明。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"

#define USJ_TX_RING_SIZE 4096u                 /* 2 的幂 */
#define USJ_TX_RING_MASK (USJ_TX_RING_SIZE - 1u)

static volatile uint8_t  s_tx_ring[USJ_TX_RING_SIZE];
static volatile uint32_t s_tx_head;            /* 生产者 */
static volatile uint32_t s_tx_tail;            /* 消费者 */

static inline uint32_t irq_save2(void)
{
    uint32_t mstatus;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    __asm__ volatile ("csrc mstatus, %0" : : "r"(0x8u));
    return mstatus;
}

static inline void irq_restore2(uint32_t f)
{
    __asm__ volatile ("csrs mstatus, %0" : : "r"(f & 0x8u));
}

/* ---- 消费者：尽量灌进 TX FIFO（主循环/中断里都能调，不阻塞）---- */
void s31_usj_pump(void)
{
    uint32_t flag = irq_save2();
    /* 🚨 循环必须有上界。环形缓冲最多就 RING_SIZE 个字节，所以"搬超过这个数"
       一定是索引被踩坏了（**栈溢出**最爱踩这里）。没有上界时的后果极严重：
       死循环疯狂写 EP1 FIFO -> **把 USB-Serial/JTAG 外设本身弄卡** ->
       esptool（也走这个口）连不上 -> 只能物理断电。
       实测踩过：换 IDF 版 PSRAM 后 8KB 栈溢出，PC 停在 0x2f001f84（就在这个函数里），
       整个板子失联，靠 JTAG 才 halt 住。加上这个上界之后，
       同类问题最多是"控制台输出花掉"，绝不会再把板子搞失联。 */
    uint32_t n = 0;

    while (s_tx_tail != s_tx_head && n < USJ_TX_RING_SIZE) {
        if (!(S31_REG32(S31_USJ_EP1_CONF) & S31_USJ_IN_EP_DATA_FREE)) {
            break;                              /* FIFO 满，下次再灌 */
        }
        S31_REG32(S31_USJ_EP1) = s_tx_ring[s_tx_tail];
        s_tx_tail = (s_tx_tail + 1u) & USJ_TX_RING_MASK;
        n++;
    }
    if (s_tx_tail != s_tx_head && n >= USJ_TX_RING_SIZE) {
        s_tx_tail = s_tx_head;                  /* 索引坏了：直接丢弃，别让它拖死系统 */
    }
    S31_REG32(S31_USJ_EP1_CONF) = S31_USJ_EP1_WR_DONE;   /* 🚨 每次都写，见文件头 */
    irq_restore2(flag);
}

/* 初始化：外设本身由 bootROM 配好，这里只把 PHY 焊盘使能再确认一遍（幂等）。
 *
 * ⚠️ 曾经试过用 "TX FIFO 空" 中断在后台自动排空发送缓冲，**不可行**：
 *    那个中断是电平型的（FIFO 一直空就一直置位），进了 ISR 清 INT_CLR 也清不掉，
 *    结果 CLIC 那一路一直 pending → 反复重入 → 主循环被饿死、日志一条都出不来。
 *    现在的做法：轮询泵 + "凡是忙等就顺手刷一下"（s31_delay_ms 里调 pump），
 *    任何带延时的死循环都能持续输出日志。 */
void s31_usj_hw_init(void)
{
    S31_REG32(S31_USJ_CONF0) |= S31_USJ_CONF0_PAD_ENABLE;
}

/* 进 trap 时硬件清 mstatus.MIE → 用它判断"我是不是在中断里" */
static int in_isr(void)
{
    uint32_t mstatus;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    return (mstatus & 0x8u) == 0;
}

static uint32_t irq_save(void)
{
    uint32_t mstatus;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    __asm__ volatile ("csrc mstatus, %0" : : "r"(0x8u));
    return mstatus;
}

static void irq_restore(uint32_t f)
{
    __asm__ volatile ("csrs mstatus, %0" : : "r"(f & 0x8u));
}

/* ---- 生产者：塞一个字节（满则丢，绝不阻塞）---- */
void s31_usj_putc(char c)
{
    int isr = in_isr();
    uint32_t flag = 0;
    uint32_t head, next;

    if (!isr) {
        flag = irq_save();
    }
    head = s_tx_head;
    next = (head + 1u) & USJ_TX_RING_MASK;
    if (next != s_tx_tail) {
        s_tx_ring[head] = (uint8_t)c;
        s_tx_head = next;
    }
    if (!isr) {
        irq_restore(flag);
    }
}
