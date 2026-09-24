/*===========================================================================
 * linker.ld -- 裸机 App（**经典模型**：LMA 在 flash，VMA 在 PSRAM）
 *
 *   ┌ 装载区 LMA 0x40100000（flash 0x100000 的映射别名）──────────────┐
 *   │ .boot / .clic_entry / .text / .rodata   ← 代码与常量的初值      │
 *   │ .data 的初值 / .sdata 的初值            ← 变量的初值            │
 *   └─────────────────────────────────────────────────────────────────┘
 *   ┌ 镜像区 VMA 0x50000000 ─────────────────────────────────────────┐
 *   │ .boot / .clic_entry / .text / .rodata   ← 运行位置（bootloader 搬）│
 *   └─────────────────────────────────────────────────────────────────┘
 *   ┌ 运行数据区 VMA 0x50800000 ─────────────────────────────────────┐
 *   │ .data / .sdata                          ← 运行位置（startup 搬）│
 *   │ .bss / .sbss                            ← 启动时清零            │
 *   │ 栈（从 0x51000000 往下长，ISR 栈再留 4KB）                       │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * ★ 谁负责什么（"经典模型"的分工）：
 *   bootloader：把 bin **整块**拷到 PSRAM 的 0x50000000（= S31_APP_FLAT_VMA），
 *               然后跳到 entry。它**不需要懂段表**（见 boot_msc_s31/README.md §3）。
 *               另外它开机就把 flash 0x100000 映到 0x40100000，
 *               这样 App 才有办法按 LMA 去读 .data 的初值。
 *   App startup：把 .data/.sdata 从 **LMA（flash）** 搬到 **VMA（PSRAM）**，
 *               再把 .bss 清零。
 *
 * ★ 为什么 LMA 能是个常量：
 *   拖进来的 bin 一定被写到 flash 的 S31_APP_LOAD_BASE = 0x100000
 *   （boot_msc_s31/app/msc_disk.c 的 data_map_off() 按"相对文件首簇"折算，
 *     所以 FAT 分了哪个簇都一样）→ 镜像起点永远是这个地址，
 *   加上映射窗口基址就是 0x40100000。
 *   ⚠️ 改 boot_msc_s31/bsp/s31_layout.h 的 S31_APP_LOAD_BASE 时，
 *      这里的 ORIGIN(FLASH) 必须跟着改（两边有 ASSERT 兜不住这种事，靠人看）。
 *
 * ★ 为什么"镜像前半段"必须放进**一个**输出段：
 *   objcopy -O binary 是按 **LMA** 排布字节的，而扁平镜像没有元数据 ——
 *   整块拷到 0x50000000 之后，"文件内偏移"就变成了"相对镜像基址的偏移"。
 *   所以 .boot/.clic/.text/.rodata 的 **LMA 偏移必须等于 VMA 偏移**。
 *   做法：把它们放进一个输出段 `.image`，段内偏移天然一一对应。
 *   （.data/.sdata 的初值虽然也在同一个 LMA 流里，但它们会被拷到
 *     0x50001xxx 这种没人用的地方 —— 无害，真正的 .data 由 startup 从
 *     LMA（flash）搬过去。所以它们不需要满足"偏移对齐"。）
 *   tools/build.ps1 里有守门检查：app.bin 的大小必须等于各段 LMA 的总跨度。
 *===========================================================================*/

ENTRY(_start)

/* ★ 装载基址（LMA 起点）—— 默认 0x40100000 = flash 0x100000 的映射别名。
 *
 *   为什么做成可覆盖的（PROVIDE = "没定义过才给默认值"）：
 *   打**容器格式**（格式 A，带段表的那个）时，镜像前面有 192 字节的头+段表，
 *   整个 payload 会往后挪 192 字节。App 的 startup.S 是按
 *   「文件内偏移 == flash 装载偏移」去读 .data 初值的，所以那种构建必须把
 *   装载基准也往后挪同样多，否则读到的就是那张段表（实测：magic 读成 0）。
 *
 *   build.ps1 做容器那一次链接时用 `--defsym=S31_LMA_BASE=0x401000C0` 传进来。
 *   ⚠️ 192 = mkapp.py 的 HDR_SIZE(64) + SEG_SIZE(16) × MAX_SEGS(8)。
 */
PROVIDE(S31_LMA_BASE = 0x40100000);

MEMORY
{
  /* 装载区（LMA）：flash 里的固定位置，经 flash 映射窗口访问。
     0x40100000 = flash 0x100000 的别名（见 boot_msc_s31/bsp/s31_layout.h）。 */
  FLASH (rwx) : ORIGIN = S31_LMA_BASE, LENGTH = 0x00800000
  /* 镜像区（VMA）：代码/常量的运行位置。bootloader 把 bin 整块拷到这里 */
  IMG  (rwx) : ORIGIN = 0x50000000, LENGTH = 0x00800000
  /* 运行数据区（VMA）：.data/.bss/栈住这儿，和镜像分开，
     这样"清零 .bss / 栈往下长"永远不会碰到镜像内容。 */
  DATA (rwx) : ORIGIN = 0x50800000, LENGTH = 0x00800000
}

SECTIONS
{
  /*===========================================================================
   * 镜像区里"偏移必须一一对应"的部分：一个输出段解决对齐问题
   *===========================================================================*/
  .image : {
    /* 入口放最前面：扁平镜像靠这一条，objcopy 输出第 0 字节 = 入口 */
    KEEP(*(.boot))

    . = ALIGN(64);                 /* CLIC 的 mtvec(mode 3) 要求 64 字节对齐 */
    KEEP(*(.clic_entry))

    . = ALIGN(4);
    __text_start = .;
    *(.text*)
    __text_end = .;

    . = ALIGN(8);
    __rodata_start = .;
    *(.rodata*)
    *(.srodata*)
    __rodata_end = .;
  } > IMG AT> FLASH
  __image_lma = LOADADDR(.image);      /* flash 装载区（0x40100000）*/
  __image_vma = ADDR(.image);          /* PSRAM 运行区（0x50000000）*/

  /*===========================================================================
   * 运行数据：VMA 在 DATA 区，**LMA（初值）留在装载区**（紧跟 .image）
   *===========================================================================*/
  .data : ALIGN(8) {
    __data_start = .;
    *(.data*)
    . = ALIGN(4);
    __data_end = .;
  } > DATA AT> FLASH
  __data_lma = LOADADDR(.data);

  .sdata : ALIGN(8) {
    __sdata_start = .;
    *(.sdata*)
    . = ALIGN(4);
    __sdata_end = .;
  } > DATA AT> FLASH
  __sdata_lma = LOADADDR(.sdata);

  .bss (NOLOAD) : ALIGN(8) {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
  } > DATA

  .sbss (NOLOAD) : ALIGN(8) {
    __sbss_start = .;
    *(.sbss*)
    *(.scommon*)
    . = ALIGN(4);
    __sbss_end = .;
  } > DATA

  PROVIDE(__bss_end = __sbss_end);

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.riscv.attributes)
  }

  /* gp：RISC-V 标准算法，落在小数据区中部（startup 里 la gp 装载） */
  PROVIDE(__global_pointer$ = MIN(__sdata_start + 0x800,
                                  MAX(__sdata_end, __sbss_end) - 0x800));
  PROVIDE(end  = __sbss_end);
  PROVIDE(_end = __sbss_end);

  /* 栈：运行数据区顶端 */
  __stack_top = ORIGIN(DATA) + LENGTH(DATA);
  __irq_stack_top = __stack_top - 0x1000;

  /* 搬运总长度：startup 用它判断"要不要搬"（也方便日志） */
  PROVIDE(__data_copy_bytes = (__data_end - __data_start) + (__sdata_end - __sdata_start));

  /* ---- 自检 ---- */
  ASSERT(ADDR(.image) == ORIGIN(IMG), " .image 的 VMA 必须从镜像区起点开始")
  ASSERT(LOADADDR(.image) == ORIGIN(FLASH), " .image 的 LMA 必须从装载区起点开始")
  ASSERT(__data_start >= ORIGIN(DATA) && __sbss_end < __stack_top,
         " .data/.bss 必须落在运行数据区且不顶到栈")
  /* 注：允许 ≤16 字节的对齐填充（ld 会把 LMA 也按段对齐，objcopy 会把它填 0，
     偏移依然一一对应，不影响正确性）*/
  ASSERT(__data_lma >= ORIGIN(FLASH) + SIZEOF(.image) &&
         __data_lma <  ORIGIN(FLASH) + SIZEOF(.image) + 16,
         " .data 的初值必须紧跟在镜像后面（装载区里）")
  ASSERT(__sdata_lma >= __data_lma + SIZEOF(.data) &&
         __sdata_lma <  __data_lma + SIZEOF(.data) + 16,
         " .sdata 的初值必须紧跟 .data")
}
