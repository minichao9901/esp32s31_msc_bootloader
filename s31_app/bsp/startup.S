/*===========================================================================
 * startup.S -- 裸机 App 的入口（由 bootloader 搬运完段之后跳进来）
 *
 * 与 bootloader 的 startup.S 的差别：
 *   - **不做** ROM 二级镜像那套（镜像头、段搬运都是 bootloader 干的）
 *   - 看门狗/超级看门狗 bootloader 已经处理过，这里只再确认一遍（幂等、便宜）
 *   - 首要任务是**立刻把 sp 换成自己的栈**：跳进来时 sp 还是 bootloader 的栈
 *===========================================================================*/

.section .boot, "ax"
.globl _start
.type _start, @function
.align 2

.set RTC_WDT_BASE,      0x20801000
.set RTC_WDT_SWD_CONF,  0x1c
.set RTC_WDT_SWD_WPROT, 0x20
.set RTC_WDT_SWD_AUTO_FEED_EN, (1 << 18)
.set WDT_WKEY,          0x50D83AA1

_start:
        /* ---- 只有 core0 干活 ---- */
        csrr    t0, mhartid
        bnez    t0, .Lpark

        /* ---- 第一件事：换自己的栈 ---- */
        la      sp, __stack_top

        /* ---- 超级看门狗自动喂狗（bootloader 已开，这里幂等再确认）---- */
        li      t0, RTC_WDT_BASE
        li      t1, WDT_WKEY
        sw      t1, RTC_WDT_SWD_WPROT(t0)
        lw      t2, RTC_WDT_SWD_CONF(t0)
        li      t3, RTC_WDT_SWD_AUTO_FEED_EN
        or      t2, t2, t3
        sw      t2, RTC_WDT_SWD_CONF(t0)
        sw      zero, RTC_WDT_SWD_WPROT(t0)

        /* ---- gp（小数据指针）：必须自己装 ---- */
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop

        /* ---- 关中断 + FPU 保持关闭（与 bootloader 一致：误用浮点立刻异常）---- */
        csrci   mstatus, 0x8

        /* ---- ISR 栈 + CLIC 陷阱入口（64 字节对齐，见 trap.S）---- */
        la      t0, __irq_stack_top
        csrw    mscratch, t0
        la      t0, s31_clic_entry
        ori     t0, t0, 3
        csrw    mtvec, t0

        /* ---- 段初始化：.data/.sdata 从 LMA 搬到 VMA，.bss/.sbss 清零 ----
         *
         * ★ 这就是"经典模型"里 App 自己要干的那一步：
         *     bootloader 只把 bin **整块**拷到 PSRAM 的 0x50000000（让入口/代码能跑），
         *     它不知道 .data 该去哪儿；变量初值还在 flash 里的 LMA 上，
         *     由这里搬进运行数据区 0x50800000。
         *
         *   链接脚本里的约定（见 linker.ld）：
         *     .data/.sdata 的初值留在**装载区**（flash 0x100000 的映射窗口
         *        0x40100000，__data_lma/__sdata_lma），
         *     运行位置在**运行数据区**（PSRAM 0x50800000，__data_start/__sdata_start）。
         *
         *   ⚠️ 能这么读前提取决于：bootloader 开机时已经把 flash 0x100000 映到
         *      0x40100000（s31_flash_mmap_window）。没有那一步，这里第一条 lw 就
         *      load access fault。
         *
         *   两个都是 4 字节对齐的（链接脚本里有 ALIGN(4)），所以按字搬。
         *   搬完再清 bss —— 顺序不能反：万一 bss 和 data 有重叠（不应该）
         *   也是先有初值再清零。
         */
        la      t0, __data_lma
        la      t1, __data_start
        la      t2, __data_end
        bgeu    t1, t2, 2f
1:      lw      t3, 0(t0)
        sw      t3, 0(t1)
        addi    t0, t0, 4
        addi    t1, t1, 4
        bltu    t1, t2, 1b
2:
        la      t0, __sdata_lma
        la      t1, __sdata_start
        la      t2, __sdata_end
        bgeu    t1, t2, 4f
3:      lw      t3, 0(t0)
        sw      t3, 0(t1)
        addi    t0, t0, 4
        addi    t1, t1, 4
        bltu    t1, t2, 3b
4:

        /* ---- 清 .bss + .sbss（bootloader 只搬有内容的段）---- */
        la      t0, __bss_start
        la      t1, __bss_end
        bgeu    t0, t1, 6f
5:      sw      zero, 0(t0)
        addi    t0, t0, 4
        bltu    t0, t1, 5b
6:
        call    app_main

.Lpark:
        wfi
        j       .Lpark

.size _start, .-_start
