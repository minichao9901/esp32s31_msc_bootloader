/*===========================================================================
 * main.c -- 裸机 App 示例（拖进 S31-BOOT 盘就能跑起来）
 *
 * ★ 本工程是 **经典 bootloader 模型** 的样板：**LMA 全在 flash，VMA 全在 PSRAM**
 *
 *     ┌ LMA（装载地址）= flash 0x100000，经映射窗口读作 0x40100000 ────────┐
 *     │ .boot/.text/.rodata 的初值  +  .data/.sdata 的初值                 │
 *     └────────────────────────────────────────────────────────────────────┘
 *     ┌ VMA（运行地址）= PSRAM 0x50000000 ────────────────────────────────┐
 *     │ .boot/.clic_entry/.text/.rodata   ← **bootloader 整块搬过来**       │
 *     └────────────────────────────────────────────────────────────────────┘
 *     ┌ VMA = PSRAM 0x50800000 ───────────────────────────────────────────┐
 *     │ .data/.sdata/.bss/栈              ← **App 自己的 startup 搬/清**    │
 *     └────────────────────────────────────────────────────────────────────┘
 *
 *   为什么必须有"flash 映射窗口"：.data 的初值留在 flash（LMA），
 *   startup.S 里 `la t0, __data_lma` 读的就是 0x401xxxxx —— 那是 flash 0x10xxxx
 *   的映射别名。bootloader 开机时把这段映好了（s31_flash_mmap_window），
 *   App 才有办法把这几个字读出来。**这是整个模型成立的前提。**
 *
 * ★ 开机只打一屏、之后串口安静（板载 LED 就是心跳）：
 *   `.image/.data` 两行给出实际的搬入地址，最后一行是布局自检的结论 ——
 *   判据 = 装载地址在 flash 段 + 数据落在 PSRAM 段 + magic/blob 值对 + .bss 为 0。
 *   值不对就是搬运坏了，不用靠现象猜。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"

int  kprintf(const char *fmt, ...);
void s31_usj_hw_init(void);
void s31_usj_pump(void);
void s31_systimer_init(void);
void s31_delay_ms(uint32_t ms);
void s31_rmt_init(void);                      /* bsp/s31_rmt.c */
int  s31_rmt_ws2812_send(const uint8_t *grb);

/* 链接脚本里定义的段边界（自检用）*/
extern char __data_start[], __data_end[];
extern char __sdata_start[], __sdata_end[];
extern char __bss_start[], __bss_end[];
/* ★ LMA：初值在 flash 装载区里的位置（startup 从这儿搬到 VMA）
 *   ⚠️ 它的具体值取决于镜像是哪种格式，别写死：
 *       扁平 app.bin           -> 0x40100000（flash 0x100000 的别名）
 *       容器 app_container.bin -> 0x401000C0（多出 192 字节头+段表，链接基准
 *                                 也跟着挪了同样多，见 bsp/linker.ld 的 S31_LMA_BASE）
 *   所以下面直接打印这个符号的值，而不是打印一个常数。 */
extern char __data_lma[], __sdata_lma[];
extern char __image_lma[], __image_vma[];

static uint32_t s_counter;                 /* .bss：应当是 0（顺便当 LED 状态机）*/
static const char s_banner[] = "  S31 bare-metal App is ALIVE";   /* .rodata */
static uint32_t s_magic = 0x1234ABCDu;     /* .sdata（小、gp 可达）*/
/* ★ 64 字节的初值数组太大，进不了 .sdata，会落到 **.data** ——
   这样 .data 和 .sdata 两条搬运路径就都被覆盖到了。
   ⚠️ 必须 volatile：否则 -O2 会把下面那处读取直接折成常量，
      数组被当成"没人用"整个删掉，.data 就永远是 0 字节（踩过）。 */
static volatile uint32_t s_data_blob[64] = {
    0x0BADF00Du, 0x00000001u, 0x00000002u, 0x00000003u,
};

/* ---- 板载 RGB LED（WS2812 @ GPIO60）----
 * 波形交给 **RMT 硬件**走 tick（为什么不能用 CPU 翻转引脚、坑在哪：
 * 见 bsp/s31_rmt.c 的文件头）。*/
static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3];
    int rc;

    grb[0] = g;                 /* WS2812 是 GRB 顺序 */
    grb[1] = r;
    grb[2] = b;

    rc = s31_rmt_ws2812_send(grb);
    if (rc != 0) {
        /* 只报一次，别把串口刷爆（RMT 坏了灯不会有任何反应，只能靠这一行）*/
        static int warned;
        if (!warned) {
            warned = 1;
            kprintf("[app] !! WS2812 发送失败 rc=%d（-1 超时 / -2 符号没进内存）\r\n", rc);
        }
    }
}

/* 经典模型的全部判据，一次判完 */
static int layout_ok(void)
{
    return (uintptr_t)__image_lma  >= 0x40000000u && (uintptr_t)__image_lma < 0x50000000u &&
           (uintptr_t)&s_magic     >= 0x50800000u &&
           s_magic == 0x1234ABCDu &&
           s_data_blob[0] == 0x0BADF00Du &&
           s_counter == 0u;
}

void app_main(void)
{
    s31_usj_hw_init();
    s31_systimer_init();
    s31_delay_ms(20);

    kprintf("\r\n================================================\r\n");
    kprintf("%s\r\n", s_banner);
    kprintf("  build %s %s\r\n", __DATE__, __TIME__);
    kprintf("------------------------------------------------\r\n");
    kprintf("[app] .image VMA %08x <- LMA %08x   [bootloader 搬]\r\n",
            (unsigned)(uintptr_t)__image_vma, (unsigned)(uintptr_t)__image_lma);
    kprintf("[app] .data  VMA %08x <- LMA %08x   [startup 从 flash 搬]\r\n",
            (unsigned)(uintptr_t)__data_start, (unsigned)(uintptr_t)__data_lma);
    kprintf("[app] 布局自检: %s\r\n", layout_ok()
                ? "OK —— LMA 在 flash、VMA 在 PSRAM，初值搬运与 .bss 清零都对"
                : "FAIL —— 段搬运不对，别往下走了");
    kprintf("[app] LED: RMT -> WS2812 (GPIO60)，红/绿/蓝 1 秒轮换\r\n");
    kprintf("================================================\r\n");

    s31_rmt_init();         /* RMT + GPIO60 路由（WS2812 全靠硬件时序）*/
    led_set(0, 0, 0);       /* 先灭灯 */

    /* 主循环：LED 就是心跳，串口不再输出。
       （要加打印的话，记得每轮调一次 s31_usj_pump() 把 FIFO 推出去。）*/
    for (;;) {
        switch (s_counter++ % 3u) {
        case 0:  led_set(32, 0, 0);  break;    /* 亮度 32/255：满亮度太刺眼 */
        case 1:  led_set(0, 32, 0);  break;
        default: led_set(0, 0, 32);  break;
        }

        s31_delay_ms(1000);
        s31_usj_pump();
    }
}
