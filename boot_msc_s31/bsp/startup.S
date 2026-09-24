/*===========================================================================
 * startup.S -- ESP32-S31 裸机启动（bootROM 直接跳到这里，没有 IDF bootloader）
 *
 * ROM 已经把镜像各段按 VMA 搬进 RAM（flash 0x2000 处的二级镜像），
 * 这里只做裸机必须自己做的事（顺序照 projects/rtt_nano_s31 的实测版本）：
 *   1. 非 0 号 hart 停住
 *   2. 关 RTC_WDT + TIMG0/1 的 MWDT
 *   3. 🚨 打开"超级看门狗"SWD 自动喂狗 —— 不喂的话 ~3.4 秒整片复位一次
 *      （因为我们在第 2 步把 RTC_WDT 关了，而 SWD 盯的就是它的喂狗信号）
 *   4. 设启动栈 / gp / mscratch(ISR 栈) / mtvec(CLIC 模式 3)
 *   5. 清 .bss + .sbss
 *   6. call main
 *===========================================================================*/

.section .boot, "ax"
.globl _start
.type _start, @function
.align 2

.set RTC_WDT_BASE,      0x20801000
.set RTC_WDT_CONFIG0,   0x00
.set RTC_WDT_WPROTECT,  0x18
.set RTC_WDT_SWD_CONF,  0x1c
.set RTC_WDT_SWD_WPROT, 0x20
.set RTC_WDT_SWD_AUTO_FEED_EN, (1 << 18)

.set TIMG0_BASE,        0x20580000
.set TIMG1_BASE,        0x20581000
.set TIMG_WDTCONFIG0,   0x48
.set TIMG_WDTWPROTECT,  0x64
.set WDT_WKEY,          0x50D83AA1

_start:
        /* ---- 只有 core0 干活，其余 hart 停住 ---- */
        csrr    t0, mhartid
        bnez    t0, .Lpark

        /* ---- 关 RTC_WDT ---- */
        li      t1, WDT_WKEY
        li      t0, RTC_WDT_BASE
        sw      t1, RTC_WDT_WPROTECT(t0)
        sw      zero, RTC_WDT_CONFIG0(t0)
        sw      zero, RTC_WDT_WPROTECT(t0)

        /* ---- 关 TIMG0/TIMG1 的 MWDT（实测 1 秒后会复位芯片）---- */
        li      t0, TIMG0_BASE
        sw      t1, TIMG_WDTWPROTECT(t0)
        sw      zero, TIMG_WDTCONFIG0(t0)
        sw      zero, TIMG_WDTWPROTECT(t0)
        li      t0, TIMG1_BASE
        sw      t1, TIMG_WDTWPROTECT(t0)
        sw      zero, TIMG_WDTCONFIG0(t0)
        sw      zero, TIMG_WDTWPROTECT(t0)

        /* ---- 🚨 超级看门狗（SWD）自动喂狗：不做这一步，板子每 ~3.4 秒复位一次 ---- */
        li      t0, RTC_WDT_BASE
        li      t1, WDT_WKEY
        sw      t1, RTC_WDT_SWD_WPROT(t0)
        lw      t2, RTC_WDT_SWD_CONF(t0)
        li      t3, RTC_WDT_SWD_AUTO_FEED_EN
        or      t2, t2, t3
        sw      t2, RTC_WDT_SWD_CONF(t0)
        sw      zero, RTC_WDT_SWD_WPROT(t0)

        /* ---- 启动栈 ---- */
        la      sp, __stack_top

        /* ---- gp：没有 crt0，必须自己装（.option norelax 防止 la 被展开成 gp 相对）---- */
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop

        /* ---- 关中断；FPU 保持关闭（FS=0）----
         * 故意不打开 FPU：这样任何浮点指令都会立刻触发 illegal-instruction 异常
         * （响亮的失败），而不是悄悄把主程序的浮点寄存器冲掉。
         * 本工程全整数，不碰 f0-f31。 */
        csrci   mstatus, 0x8                    /* MIE = 0 */

        /* ---- ISR 栈 + CLIC 陷阱入口 ---- */
        la      t0, __irq_stack_top
        csrw    mscratch, t0                    /* trap 入口用 csrrw 换栈 */
        la      t0, s31_clic_entry
        ori     t0, t0, 3                       /* MTVEC_MODE_CSR = 3 (CLIC) */
        csrw    mtvec, t0

        /* ---- 清 .bss + .sbss ---- */
        la      t0, __bss_start
        la      t1, __bss_end
        bgeu    t0, t1, 2f
1:      sw      zero, 0(t0)
        addi    t0, t0, 4
        bltu    t0, t1, 1b
2:
        call    main

.Lpark:
        wfi
        j       .Lpark

.size _start, .-_start
