/*===========================================================================
 * s31_flash.c -- 板载 flash 的读写擦（走 ROM 的 legacy spi_flash API）
 *
 * 用 ROM 里的 `esp_rom_spiflash_read/write/erase_sector`（地址在 linker.ld 里
 * PROVIDE 出来）而不是 IDF 那套 esp_flash_t：
 *   - 它们**不需要任何结构体**（裸机最省事），签名简单；
 *   - 是 ROM 自己的代码，跑在内部 ROM 里，操作期间不碰 flash；
 *   - IDF 的二级 bootloader 用的就是这一套。
 *
 * 🚨 写/擦期间必须让 CPU **完全不碰 flash**：
 *   SPI1（SPIMEM0）既服务 cache 又服务这几个 ROM 函数，操作期间若有 cache miss
 *   去读 flash，就会互相踩。做法：**关中断**（ISR 里可能访问 flash 里的 const 数据）
 *   + 全程只用内部 RAM 的缓冲。本工程所有代码/数据都在内部 RAM（链接脚本保证）。
 *   （要不要连 cache 一起挂起 —— 实测**不需要**，见下面那段的说明。）
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"
#include "s31_layout.h"
#include "hal/mmu_ll.h"             /* IDF：MMU 页表 LL（flash 映射窗口）*/

/* ROM 函数（地址见 bsp/linker.ld 的 PROVIDE）*/
extern int esp_rom_spiflash_read(uint32_t src_addr, uint32_t *dest, int32_t len);
extern int esp_rom_spiflash_write(uint32_t dest_addr, const uint32_t *src, int32_t len);
extern int esp_rom_spiflash_erase_sector(uint32_t sector_num);
extern int esp_rom_spiflash_unlock(void);
extern int esp_rom_spiflash_config_param(uint32_t deviceId, uint32_t chip_size, uint32_t block_size,
                                         uint32_t sector_size, uint32_t page_size, uint32_t status_mask);

/* ROM 的 flash 芯片结构体指针（反汇编 0x2f80ef70 得到：
 *   原始 erase 会先做 `sector_num < chip_size/sector_size` 的范围检查，
 *   不满足就直接返回 ERR **且根本不碰 flash**）。 */
#define S31_ROM_FLASHCHIP_PTR 0x2f07ffe0u

/* ROM cache 控制（rom/cache.h）：作废映射窗口里的 cache 行用 */
extern int Cache_WriteBack_Invalidate_Addr(uint32_t map, uint32_t addr, uint32_t size);

#define CACHE_MAP_L1_DCACHE 0x2u   /* cache.h: CACHE_MAP_L1_DCACHE */

static inline uint32_t irq_save(void)
{
    uint32_t mstatus;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    __asm__ volatile ("csrc mstatus, %0" : : "r"(0x8u));
    return mstatus;
}

static inline void irq_restore(uint32_t f)
{
    __asm__ volatile ("csrs mstatus, %0" : : "r"(f & 0x8u));
}

void s31_flash_init(void)
{
    /* 🚨 bootROM 交棒时 g_rom_flashchip.chip_size 只有 **2MB**（实测：
     *    device_id=0x00464018 但 chip_size=2097152 / 512 个扇区）。
     *    而 ROM 的擦/写/读都会先做范围检查 —— 超出就直接返回 ERR 且不碰 flash，
     *    现象是"擦失败、写完读回全是 0"，极容易被误判成"cache 挂起把 SPI1 搞坏了"。
     *    解法就是 IDF 自己用的这个官方接口，把真实容量告诉 ROM。 */
    uint32_t chip = S31_REG32(S31_ROM_FLASHCHIP_PTR);
    uint32_t device_id = chip ? S31_REG32(chip + 0) : 0;

    esp_rom_spiflash_config_param(device_id, S31_FLASH_SIZE, 64u * 1024u,
                                  4096u, 256u, 0xFFFFu);

    /* 清写保护位（块保护）；ROM 已经配好 SPI1 的读模式，不需要再动 */
    esp_rom_spiflash_unlock();
}

int s31_flash_read(uint32_t addr, void *dst, uint32_t len)
{
    return esp_rom_spiflash_read(addr, (uint32_t *)dst, (int32_t)len);
}

/* 写/擦期间要不要挂起 cache？—— **实测结论：不需要**（2026-09-24，S31 真板）。
 *   - ROM 的 `esp_rom_spiflash_*` 是"包装版"，它自己会处理 SPI1 与 cache 的关系；
 *   - 本工程所有代码/数据都在内部 RAM，flash 操作期间不会有 flash 取指/取数；
 *   - 曾怀疑"擦失败是 cache 挂起搞坏的"，实际真因是 ROM 的 chip_size 只有 2MB
 *     导致的范围检查（见 s31_flash_init 注释）。
 * 所以这里只关中断（ISR 里可能碰 flash 里的 const 数据）。 */

int s31_flash_erase_sector(uint32_t addr)
{
    uint32_t flag = irq_save();
    int r = esp_rom_spiflash_erase_sector(addr / 4096u);

    irq_restore(flag);
    return r;
}

/* 写：调用者必须保证目标已擦除；addr 与 len 都要求 4 字节对齐 */
int s31_flash_write(uint32_t addr, const void *src, uint32_t len)
{
    uint32_t flag = irq_save();
    int r = esp_rom_spiflash_write(addr, (const uint32_t *)src, (int32_t)len);

    irq_restore(flag);
    return r;
}

/*===========================================================================
 * flash 映射窗口（0x40000000 起，flash 物理地址 X 读作 0x40000000+X）
 *
 * ★ 为什么 App 需要它：
 *   App 镜像的 **LMA 全在 flash**（.text/.data 都是），VMA 在 PSRAM。
 *   bootloader 把 .text 搬到 PSRAM 让它能跑；跑起来之后 App 的 startup.S 还要
 *   按 `__data_lma`（= S31_APP_LMA_BASE + 文件内偏移）去读 .data 的初值 ——
 *   那个地址就是这里的窗口地址。
 *   所以开机时**必须**把 App 装载区映射好，否则 App 一读 .data 就 access fault。
 *
 * ★ 这一段为什么放在 bootloader 里做（而不是让 App 自己做）：
 *   MMU 是 SPI_MEM_C/S 的寄存器。App 里再写一遍也行，但那样每个 App 都得抄一遍；
 *   bootloader 做一次，App 就只管 "la t0, __data_lma"。
 *
 * ⚠️ 页大小由 ROM 决定（MMU_PAGE_256KB/128KB/64KB/32KB 之一），
 *    用 mmu_ll_get_page_size() 读回来按实际值算 —— 别写死。
 * ⚠️ 不碰 bootloader 自己那块映射：ROM 映的是 flash 0x0 起的低区，
 *    我们映的是 0x100000 起的高区，页号不会重合。
 *===========================================================================*/

/* 把 [flash_off, flash_off+len) 映射到窗口地址 (S31_FLASH_MMAP_BASE + flash_off)。
 * 返回映射的页数，<0 表示失败（页大小读到 0）。 */
int s31_flash_mmap_window(uint32_t flash_off, uint32_t len)
{
    mmu_page_size_t page_size = mmu_ll_get_page_size(MMU_LL_FLASH_MMU_ID);
    uint32_t vaddr = S31_FLASH_MMAP_BASE + flash_off;
    uint32_t end = vaddr + len;
    uint32_t pages = 0;

    if (page_size == 0) {
        return -1;
    }
    /* 起始地址按页对齐（装载区本身是 1MB 对齐的，这里只是防御）*/
    vaddr &= ~(uint32_t)(page_size - 1u);

    while (vaddr < end) {
        uint32_t paddr = vaddr - S31_FLASH_MMAP_BASE;
        uint32_t entry = mmu_ll_get_entry_id(MMU_LL_FLASH_MMU_ID, vaddr);
        uint32_t val = mmu_ll_format_paddr(MMU_LL_FLASH_MMU_ID, paddr, MMU_TARGET_FLASH0);

        mmu_ll_write_entry(MMU_LL_FLASH_MMU_ID, entry, val, MMU_TARGET_FLASH0);
        vaddr += page_size;
        pages++;
    }
    return (int)pages;
}

/* 作废映射窗口里的一段 cache 行。
 *
 * ★ 必须做：镜像是用 ROM 的 SPI1 直接写进 flash 的（**从 cache 底下穿过去**），
 *   而映射窗口是**可 cache** 的。如果之前有人读过这段（比如开机校验时），
 *   cache 里就留着旧行，App 读 .data 会拿到陈旧内容。
 *   写点用 WriteBack+Invalidate：万一窗口里有脏行（不该有），也先落盘再丢。 */
void s31_flash_mmap_invalidate(uint32_t flash_off, uint32_t len)
{
    Cache_WriteBack_Invalidate_Addr(CACHE_MAP_L1_DCACHE,
                                    S31_FLASH_MMAP_BASE + flash_off, len);
}
