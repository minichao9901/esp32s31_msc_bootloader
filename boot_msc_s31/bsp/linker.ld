/*===========================================================================
 * linker.ld -- ESP32-S31 裸机 MSC bootloader
 *
 * 内存窗口（IDF soc.h:152 + ld.hp_mem_defs:9，rtt_nano_s31 实测过）：
 *   0x2F000000 .. 0x2F07AFC0   491456 B  (SRAM_SEG_END = 二级 bootloader 保留区起点)
 *
 * 布局（自底向上）：
 *   .boot / .clic_entry(64B 对齐) / .text / .rodata / .data / .sdata
 *   .bss / .sbss                        ← 启动时清零
 *   .noinit                             ← 跨复位保留（启动模式标志等）
 *   栈（ISR 4KB + 主 8KB）               ← 静态数组，见下
 *   ↓
 *   __bl_end 必须 < APP_IRAM_BASE (0x2F060000)
 *
 * ⚠️ 为什么要给 App 留 RAM：bootloader 要把 App 的"内部 RAM 段"从 flash 搬进来，
 *    如果 App 段落在 bootloader 自己的代码/数据/栈上，等于边跑边把自己覆盖掉。
 *    所以约定 App 的内部 RAM 段只能落在 [APP_IRAM_BASE, 0x2F07AFC0)。
 *
 * ⚠️ 启动栈/ISR 栈用**静态数组**（.bss 里），不放在窗口顶端 ——
 *    窗口顶端要留给 App。
 *===========================================================================*/

ENTRY(_start)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x2F000000, LENGTH = 0x60000   /* 384KB 给 bootloader */
}

SECTIONS
{
  .boot : {
    KEEP(*(.boot))
  } > RAM

  /* CLIC 陷阱入口跳板：mtvec[31:6] 是基址 → 必须 64 字节对齐 */
  .clic_entry : {
    . = ALIGN(64);
    KEEP(*(.clic_entry))
  } > RAM

  .text : ALIGN(4) {
    *(.text*)
  } > RAM

  .rodata : ALIGN(8) {
    *(.rodata*)
    *(.srodata*)
  } > RAM

  .data : ALIGN(8) {
    *(.data*)
  } > RAM

  .sdata : ALIGN(8) {
    __sdata_start = .;
    *(.sdata*)
    __sdata_end = .;
  } > RAM

  .bss (NOLOAD) : ALIGN(8) {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
  } > RAM

  .sbss (NOLOAD) : ALIGN(8) {
    __sbss_start = .;
    *(.sbss*)
    *(.scommon*)
    . = ALIGN(8);
    __sbss_end = .;
  } > RAM

  /* ---- 栈：两段静态预留 ----
   *   主栈   8KB  [__stack_bottom, __stack_top)     向下生长
   *   ISR 栈 4KB  [__stack_top,    __irq_stack_top) 向下生长（trap.S 换进来）
   * ⚠️ 不放在 RAM 窗口顶端 —— 顶端那片要留给 App 的内部 RAM 段。 */
  .stack (NOLOAD) : ALIGN(16) {
    __stack_bottom = .;
    /* 🚨 8KB → 32KB：IDF 的 esp_psram_impl_enable() 调用层次深、局部变量大，
       8KB 会溢出。**栈溢出在这个工程里的表现极隐蔽**：它踩坏的是 USJ 发送环形缓冲的
       读写索引，于是 s31_usj_pump() 里 `while (tail != head)` 永远转下去 ——
       现象是"PC 停在 s31_usj_pump 里、控制台一个字节都不出"，很容易误判成 USB/串口问题。
       （实测踩过：换成 IDF 版 PSRAM 后开机 0 字节输出，PC=0x2f001f84 落在 pump 内。）*/
    . += 0x8000;
    __stack_top = .;
    . += 0x2000;                                    /* ISR 栈 4KB → 8KB */
    __irq_stack_top = .;
  } > RAM

  /* ---- 只读的"表"必须放在 .bss 之前 ---- */

  /* ---- 跨复位保留区（bootloader 重启后还要用的东西）----
     ⚠️ 必须放在 __bss_end 之后（否则被清零），也不能被别的东西覆盖。 */
  .noinit (NOLOAD) : ALIGN(8) {
    __noinit_start = .;
    KEEP(*(.noinit))
    KEEP(*(.noinit*))
    . = ALIGN(8);
    __noinit_end = .;
  } > RAM

  /* 清零范围 = .bss + .sbss（两者紧邻）。千万不要把 __bss_end 指到 .noinit 后面。 */
  PROVIDE(__bss_end = __sbss_end);

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.riscv.attributes)
  }

  /* ---- gp（小数据指针）：没有 crt0，必须自己给符号 + startup 里装 ---- */
  PROVIDE(__global_pointer$ = MIN(__sdata_start + 0x800,
                                  MAX(__sdata_end, __sbss_end) - 0x800));

  PROVIDE(end  = __sbss_end);
  PROVIDE(_end = __sbss_end);

  /* ---- bootloader 边界（给 App 搬运代码用）---- */
  __bl_end = .;
  ASSERT(__bl_end <= 0x2F060000, "BOOTLOADER TOO BIG: image must stay below S31_APP_IRAM_BASE (0x2F060000)")
  ASSERT(__irq_stack_top <= 0x2F060000, "STACKS OVERLAP APP IRAM AREA (0x2F060000)")

  /*=========================================================================
   * ROM 函数符号：我们不带 IDF，直接把这些入口地址 PROVIDE 出来，
   * C 里 extern 声明即可调用（地址出自 components/esp_rom/esp32s31/ld/*.ld）
   *=======================================================================*/
  /* --- flash 读写擦（SPI1 memspi，legacy ROM API：不要结构体，最适合裸机）---
     地址出自 components/esp_rom/esp32s31/ld/esp32s31.rom.ld --- */
  PROVIDE(esp_rom_spiflash_read          = 0x2f80016c);
  PROVIDE(esp_rom_spiflash_write         = 0x2f800168);
  PROVIDE(esp_rom_spiflash_erase_sector  = 0x2f800160);
  PROVIDE(esp_rom_spiflash_erase_block   = 0x2f800164);
  PROVIDE(esp_rom_spiflash_unlock        = 0x2f800170);
  PROVIDE(esp_rom_spiflash_config_param  = 0x2f80017c);

  /* --- ROM 的 octal PSRAM/flash 事务接口（rom/opi_flash.h）--- */
  PROVIDE(esp_rom_spi_set_op_mode = 0x2f800124);
  PROVIDE(esp_rom_spi_cmd_config  = 0x2f80011c);
  PROVIDE(esp_rom_spi_cmd_start   = 0x2f800120);

  /* --- 外设实例符号 ---
     把 IDF 的 LL 头文件（bsp/idf_port/hal/*.h）原样拿来用时，它们引用的是
     `extern xxx_dev_t SPIMEM2;` 这种"实例符号" —— IDF 靠 peripherals.ld 提供地址。
     我们照抄一份，地址出自 components/soc/esp32s31/ld/esp32s31.peripherals.ld。
     （手抄寄存器容易错，直接用 IDF 的 LL 才是正路 —— 本项目为此走了两轮弯路。）*/
  PROVIDE(SPIMEM2       = 0x20502000);
  PROVIDE(SPIMEM3       = 0x20503000);
  PROVIDE(MSPI_IOMUX    = 0x20584000);
  PROVIDE(HP_SYSTEM     = 0x20586000);
  PROVIDE(HP_SYS_CLKRST = 0x20587000);
  PROVIDE(HP_ALIVE_SYS  = 0x20589000);
  PROVIDE(LP_SYS        = 0x20700000);
  PROVIDE(PMU           = 0x20704000);
  /* hal/mmu_ll.h -> mmu_ll_cache_encryption_enabled() 读 EFUSE.rd_repeat_data1，
     只为判断 flash 加密有没有开（我们没开）。符号地址出自 IDF 的
     components/soc/esp32s31/ld/esp32s31.peripherals.ld:102。 */
  PROVIDE(EFUSE         = 0x20715000);
  PROVIDE(CACHE         = 0x2C000000);   /* soc/esp32s31/ld/esp32s31.peripherals.ld */

  /* --- cache / MMU（PSRAM 映射与 flash 操作时挂起 cache；rom/cache.h）--- */
  PROVIDE(Cache_PSRAM_MMU_Init          = 0x2f8006c0);
  PROVIDE(Cache_PSRAM_MMU_Set           = 0x2f8006cc);
  PROVIDE(Cache_Disable_L1_CORE0_ICache = 0x2f80068c);
  PROVIDE(Cache_Enable_L1_CORE0_ICache  = 0x2f800690);
  PROVIDE(Cache_Disable_L1_DCache       = 0x2f8006ac);
  PROVIDE(Cache_Enable_L1_DCache        = 0x2f8006b0);
  PROVIDE(Cache_Suspend_L1_DCache       = 0x2f8006b4);
  PROVIDE(Cache_Resume_L1_DCache        = 0x2f8006b8);
  PROVIDE(Cache_Suspend_L1_CORE0_ICache = 0x2f800694);
  PROVIDE(Cache_Resume_L1_CORE0_ICache  = 0x2f800698);
  PROVIDE(Cache_Invalidate_Addr         = 0x2f8005e8);
  PROVIDE(Cache_WriteBack_Addr          = 0x2f8005f0);
  PROVIDE(Cache_WriteBack_Invalidate_Addr = 0x2f8005f4);
  PROVIDE(Cache_Invalidate_All          = 0x2f8005f8);
  PROVIDE(Cache_WriteBack_All           = 0x2f800600);
}
