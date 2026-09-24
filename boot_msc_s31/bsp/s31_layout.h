/*===========================================================================
 * s31_layout.h -- flash 布局与 App 镜像格式（bootloader 与 App 共用）
 *
 * Flash（16MB，QIO@80MHz）：
 *   0x000000 ┌──────────────────────────────┐
 *            │ ROM 保留 / 分区表区（不用）   │
 *   0x002000 ├──────────────────────────────┤
 *            │ 本 bootloader 镜像            │ ← ROM 从这里加载二级镜像
 *            │ （留到 0x0FFFFF，实际 ~100KB）│
 *   0x100000 ├──────────────────────────────┤
 *            │ ★ App 镜像装载区（8MB）       │ ← 拖进来的 bin **固定**落在这里
 *            │   （同时就是虚拟磁盘的数据区）│    同时被映射到 0x40100000 当 App 的 LMA
 *   0x900000 ├──────────────────────────────┤
 *            │ App 描述符块（4KB）           │ ← 写入完成后填，开机据此判断"有没有程序"
 *   0x901000 ├──────────────────────────────┤
 *            │ 空闲                          │
 *   0xF00000 ├──────────────────────────────┤
 *            │ 自检/临时区                   │
 *   0x1000000└──────────────────────────────┘
 *
 * ★ 为什么镜像必须落在**固定**偏移（经典 bootloader 模型的关键）：
 *   App 镜像里 .text 和 .data 的**装载地址（LMA）都在 flash**、**运行地址（VMA）都在 PSRAM**：
 *       .text/.rodata   LMA=flash  VMA=PSRAM   ← bootloader 搬到 PSRAM 才能跑
 *       .data/.sdata    LMA=flash  VMA=PSRAM   ← App 自己的 startup.S 搬
 *   而"LMA 是哪个地址"必须是**链接期常量** —— 否则 App 的 startup.S 没法用一条
 *   `la t0, __data_lma` 去读 .data 的初值。
 *   ⇒ 所以拖进来的 bin 一律写到 S31_APP_LOAD_BASE（= 0x100000），
 *     App 链接时就把 LMA 定成 S31_APP_LMA_BASE = 0x40100000。
 *
 *   这是参考 STM32 那版 `cherry_drag_n_drop_bootloader_fat16` 的做法：
 *   它把数据扇区**按文件内偏移**线性写到固定地址 VFAT16_FLASH_START_ADDR。
 *   我们这里等价地做在簇映射上（msc_disk.c 的 data_map_off）。
 *===========================================================================*/
#ifndef S31_LAYOUT_H
#define S31_LAYOUT_H

#include <stdint.h>

#define S31_FLASH_SIZE        0x1000000u   /* 16MB */
#define S31_FLASH_SECTOR      0x1000u      /* 4KB */
#define S31_FLASH_BLOCK       0x10000u     /* 64KB */

#define S31_BL_FLASH_OFFSET   0x2000u      /* bootloader 镜像偏移（ROM 二级镜像）*/
#define S31_BL_MAX_SIZE       0xF0000u     /* 0x2000..0x100000，留足空间 */

#define S31_APP_DATA_BASE     0x100000u    /* 磁盘数据区起点 */
#define S31_APP_DATA_SIZE     0x800000u    /* 8MB */

#define S31_APP_DESC_OFFSET   0x900000u    /* App 描述符块（独占 4KB）*/

/* ---- App 镜像装载区（★ 固定地址，见文件头说明）----
 * 拖进来的 bin 一律写到这里（不管 FAT 分了哪个簇，见 msc_disk.c 的 data_map_off），
 * 开机时 bootloader 把这段映射进 flash 映射窗口（s31_flash_mmap_window），
 * App 于是能按 S31_APP_LMA_BASE 直接读自己的 .text/.data 初值。
 *
 * 现在就等于磁盘数据区起点 —— 两者本来就是同一段 flash，没必要再分一块。 */
#define S31_APP_LOAD_BASE      S31_APP_DATA_BASE                            /* 0x00100000 */
#define S31_APP_LOAD_SIZE      S31_APP_DATA_SIZE                            /* 最大 8MB */

/* flash 映射窗口：flash 物理地址 X 在这里读作 (S31_FLASH_MMAP_BASE + X)。
 * App 的 LMA 就是按这个窗口链接的（见 s31_app/bsp/linker.ld 的 ORIGIN(FLASH)）。 */
#define S31_FLASH_MMAP_BASE   0x40000000u
#define S31_APP_LMA_BASE      (S31_FLASH_MMAP_BASE + S31_APP_LOAD_BASE)     /* 0x40100000 */

/* ---- App 镜像头（拖进去的那个 app.bin 的开头就是它）----
 *
 * 段表描述"从 flash 哪里搬到内存哪里"：
 *   - seg[i].off  = 相对**镜像起点**的偏移（镜像固定在 flash 的 S31_APP_LOAD_BASE）
 *   - seg[i].vma  = 目标虚拟地址（PSRAM 0x50000000 或内部 RAM [0x2F060000, 0x2F07AFC0)）
 *   - seg[i].size = 字节数
 * 搬完之后跳到 entry。
 */
#define S31_APP_MAGIC      0x41313353u   /* 'S31A' */

#define S31_APP_MAX_SEGS   8

typedef struct {
    uint32_t off;          /* 相对镜像起点的偏移 */
    uint32_t vma;          /* 目标地址 */
    uint32_t size;         /* 字节数 */
    uint32_t flags;        /* bit0: 全 0 填充（bss），不从 flash 搬 */
} s31_app_seg_t;

typedef struct {
    uint32_t magic;        /* S31_APP_MAGIC */
    uint32_t version;      /* 格式版本 = 1 */
    uint32_t entry;        /* 入口地址（VMA）*/
    uint32_t nsegs;        /* 段数（<= S31_APP_MAX_SEGS）*/
    uint32_t total_size;   /* 镜像总字节数（含头），用于 CRC 与长度校验 */
    uint32_t crc32;        /* 覆盖 [0, total_size)，crc 字段本身按 0 计入 */
    uint32_t build_time;   /* 可选：构建时间戳 */
    uint32_t reserved[9];  /* 凑到 64 字节，后面接段表 */
    s31_app_seg_t segs[S31_APP_MAX_SEGS];
} s31_app_header_t;

/* ---- App 描述符（flash 0x500000 那个独立扇区）----
 * bootloader 每次开机读它：magic 对 + 镜像 CRC 对 => 有程序可跑。 */
#define S31_APPDESC_MAGIC  0x44433353u   /* 'S3CD' */

typedef struct {
    uint32_t magic;        /* S31_APPDESC_MAGIC */
    uint32_t version;      /* = 1 */
    uint32_t src_off;      /* ⚠️ 现在**恒为 0**：镜像固定在 S31_APP_LOAD_BASE，
                            *    不再"跟着 FAT 簇走"了。留着这个字段只为对齐历史格式。 */
    uint32_t img_size;     /* 镜像字节数（= 拖进来那个文件的长度）*/
    uint32_t img_crc;      /* 镜像 CRC32 —— ★ 扁平格式**唯一的防线**，
                            *   开机 app_probe() 必须拿它跟现算的 CRC 比对 */
    uint32_t entry;        /* 冗余存一份入口，方便日志 */
    uint32_t state;        /* 见下 */
    uint32_t name_crc;     /* 文件名（可选）*/
    uint32_t reserved[7];
    uint32_t desc_crc;     /* 覆盖本结构前 15 个字的 CRC32 */
} s31_appdesc_t;

#define S31_APPDESC_STATE_EMPTY  0u   /* 没有程序 */
#define S31_APPDESC_STATE_WRITING 1u  /* 正在写（拖拽中）*/
#define S31_APPDESC_STATE_READY  2u   /* 写好了，可启动 */
#define S31_APPDESC_STATE_BAD    3u   /* 校验失败（拖进来的不是合法镜像）*/

/*===========================================================================
 * ★ 支持的**两种**镜像格式（bootloader 按开头 4 字节自动识别）
 *
 * 【格式 A】带段表的容器 —— `s31_app/tools/mkapp.py` 产出，以 magic 'S31A' 开头
 *     64B 头 + 8×16B 段表 + 各段 payload；能表达"多区域 VMA / 精确入口 / bss 清零段"。
 *     ⚠️ 它是为"多区域 VMA"准备的**兼容路径**，现在没有工程在用它。
 *
 * 【格式 B】**扁平的 objcopy 产物**（★ 推荐，`s31_app` 默认就产这个）
 *         riscv32-esp-elf-objcopy -O binary app.elf app.bin
 *     · 镜像第 0 字节装在 S31_APP_FLAT_VMA（= PSRAM 0x50000000），入口也在那儿
 *     · 长度 = 文件长度（FAT 目录里那个 size）
 *     · `.bss` 由 App 自己的 `startup.S` 清零（objcopy 本来就不输出 NOBITS）
 *     · **没有任何元数据** ⇒ 校验只能靠描述符里的 `img_crc`（见上面）
 *
 * 为什么"整块拷到 S31_APP_FLAT_VMA"就够了（经典模型，详见 README §3.0）：
 *     App 的 LMA 是 [.boot .clic .text .rodata][.data/.sdata 初值] 一条连续的流，
 *     VMA 是 [同上，基址 0x50000000] + [.data/.sdata 在 0x50800000]。
 *     整块拷过去 ⇒ 前半段（入口/代码/常量）**偏移天然对齐**，原地就能跑；
 *     后半段（变量初值）被拷到 0x50001xxx 这种**没人用的地方**（无害），
 *     真正生效的是 App 的 startup.S 从 flash 里的 LMA 那次搬运。
 *     ⇒ bootloader **完全不用懂段表**，只要"bin 多大就拷多大"。
 *
 * ⚠️ 判断依据是"开头 4 字节 == 'S31A'"，**其他一律当格式 B** ——
 *     所以拖一个完全无关的 .bin 进来也会被装到 S31_APP_FLAT_VMA 并跳过去（跑飞）。
 *     恢复很简单：**按住 GPIO0（J2-9 接 GND）上电**就进 bootloader 模式，重拖一个即可。
 *     想彻底避免：把描述符块 0x900000 擦成 0xFF（见 README §7 的救场命令）。
 *
 * 🚨 格式 B 唯一的**硬约束**：镜像**前半段**（bootloader 要整块拷过去的那些）
 *     必须满足「文件内偏移 == 相对 S31_APP_FLAT_VMA 的偏移」。
 *     objcopy 是按 **LMA** 排布字节的，所以做法是把它们放进**一个**输出段：
 *         .image : { ... } > IMG AT> FLASH
 *     不能按段分别写 `> IMG AT> FLASH`：ld 会按"上一段 LMA + 段大小"紧凑分配
 *     LMA、**不镜像 VMA 侧的对齐空隙**，扁平镜像就跟 VMA 错位了（实测踩过，
 *     `.clic_entry` 差了 62 字节 → `.text` 整体错位 → 必崩）。
 *     `s31_app/tools/build.ps1` 里有守门检查：app.bin 大小必须等于各段 LMA
 *     的总跨度，改坏了直接编译失败。
 *===========================================================================*/

/* 扁平镜像的装载地址 / 入口：镜像第 0 字节搬到这里，也从这里开始跑。
 * 经典模型下它就是 PSRAM 窗口起点 —— App 的 .text VMA。
 *   （App 自己的 LMA 是 S31_APP_LMA_BASE，那只跟 App 内部寻址有关，
 *     bootloader 不需要知道。）
 * 需要同时支持两种以上布局的话就该用格式 A（容器）了。 */
#define S31_APP_FLAT_VMA     S31_PSRAM_VADDR

#endif /* S31_LAYOUT_H */
