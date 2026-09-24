/*===========================================================================
 * trap.S -- CLIC 陷阱入口（64 字节对齐）+ 现场保存 → C 分发
 *
 * 【为什么要 64 字节对齐】CLIC 下 mtvec[31:6] 是基址字段，CPU 会跳到
 *   mtvec[31:6]<<6；入口不在 64 字节边界上就会被取整到别处。
 *
 * 【换栈】mscratch 常驻"ISR 栈顶"，进来先 csrrw 交换：
 *   进入时 sp 指向被中断的栈（存进 mscratch），sp 变成 ISR 栈；
 *   返回前再交换回去。这样 ISR 不会吃掉主程序的栈。
 *
 * 【保存哪些寄存器】ra + t0..t6 + a0..a7（16 个 = 64 字节）。
 *   这些是 caller-saved，C 函数会随意破坏，必须保存。
 *   s0..s11 由 C 编译器自己保证，不用管。
 *   FPU 保持关闭（mstatus.FS=0），所以不存在浮点寄存器被冲掉的问题。
 *===========================================================================*/

.section .clic_entry, "ax"
.balign 64
.globl s31_clic_entry
.type s31_clic_entry, @function
s31_clic_entry:
        csrrw   sp, mscratch, sp            /* sp <-> mscratch：换成 ISR 栈 */
        addi    sp, sp, -64
        sw      ra,  0(sp)
        sw      t0,  4(sp)
        sw      t1,  8(sp)
        sw      t2, 12(sp)
        sw      a0, 16(sp)
        sw      a1, 20(sp)
        sw      a2, 24(sp)
        sw      a3, 28(sp)
        sw      a4, 32(sp)
        sw      a5, 36(sp)
        sw      a6, 40(sp)
        sw      a7, 44(sp)
        sw      t3, 48(sp)
        sw      t4, 52(sp)
        sw      t5, 56(sp)
        sw      t6, 60(sp)

        csrr    a0, mcause
        csrr    a1, mepc
        call    s31_trap_dispatch

        lw      ra,  0(sp)
        lw      t0,  4(sp)
        lw      t1,  8(sp)
        lw      t2, 12(sp)
        lw      a0, 16(sp)
        lw      a1, 20(sp)
        lw      a2, 24(sp)
        lw      a3, 28(sp)
        lw      a4, 32(sp)
        lw      a5, 36(sp)
        lw      a6, 40(sp)
        lw      a7, 44(sp)
        lw      t3, 48(sp)
        lw      t4, 52(sp)
        lw      t5, 56(sp)
        lw      t6, 60(sp)
        addi    sp, sp, 64
        csrrw   sp, mscratch, sp            /* 换回被中断的栈 */
        mret
.size s31_clic_entry, . - s31_clic_entry
