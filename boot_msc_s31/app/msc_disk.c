/*===========================================================================
 * msc_disk.c -- 虚拟 FAT16 磁盘：把"拖拽进来的文件"直接写进 flash
 *
 * 设计（对照参考工程 stm32f103_cherryusb_drag_n_drop_iap，但做了两处关键改进）：
 *
 *  ① 元数据在 RAM，数据区 1:1 映射 flash。
 *     引导扇区 / 两张 FAT / 根目录都放在 RAM 里（约 33KB），开机重建；
 *     只有"数据区"的读写才真的落到 flash 的 App 数据区。
 *     好处：FAT 更新不走 flash（否则每个簇分配都要擦写一次 flash，慢且伤寿命），
 *           而且开机永远是"空盘"，用户每次拖进来的文件都从数据区 0 偏移开始。
 *
 *  ② 数据写入走 **4KB 写回缓存**（flash 的擦除粒度）。
 *     Windows 按 512B 扇区写，直接落盘会变成"每 512B 擦一次 4KB"；
 *     缓存攒满一个 4KB 块再擦+写，实测快一个数量级。
 *     块内首次写入时先把旧内容读进来（read-modify-write），保证部分写也正确。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"
#include "s31_layout.h"
#include "usbd_core.h"        /* 里面会引 usb_config.h + usb_util.h（__PACKED/WBVAL 的来源）*/
#include "usbd_msc.h"

int kprintf(const char *fmt, ...);
int  s31_flash_read(uint32_t addr, void *dst, uint32_t len);
int  s31_flash_write(uint32_t addr, const void *src, uint32_t len);
int  s31_flash_erase_sector(uint32_t addr);

/*===========================================================================
 * 卷几何（改这里就能改容量；cluster 数必须 >= 4085，否则就不是 FAT16 了）
 *===========================================================================*/
#define DISK_BLOCK       512u
#define CLU_SECTORS      2u                              /* 2×512 = 1KB 簇 */
#define CLU_SIZE         (DISK_BLOCK * CLU_SECTORS)
#define DISK_DATA_BYTES  S31_APP_DATA_SIZE               /* 8MB */
#define DATA_SECTORS     (DISK_DATA_BYTES / DISK_BLOCK)  /* 16384 */
#define CLU_COUNT        (DATA_SECTORS / CLU_SECTORS)    /* 8192 簇 */

#define RESERVED_SECT   1u
#define FAT_SECTORS      33u     /* ceil((8192+2)*2 / 512) = 33 */
#define ROOT_ENTRIES     512u
#define ROOT_SECTORS     ((ROOT_ENTRIES * 32u) / DISK_BLOCK)   /* 32 */

#define FAT1_LBA         RESERVED_SECT
#define FAT2_LBA         (FAT1_LBA + FAT_SECTORS)
#define ROOT_LBA         (FAT2_LBA + FAT_SECTORS)
#define DATA_LBA         (ROOT_LBA + ROOT_SECTORS)
#define TOTAL_SECTORS    (DATA_LBA + DATA_SECTORS)

#define CLU_FIRST        2u                       /* 第一个数据簇 */
#define CLU_LAST         (CLU_FIRST + CLU_COUNT - 1u)
#define CLU_EOC          0xFFFFu

/*===========================================================================
 * RAM 里的元数据
 *===========================================================================*/
static uint8_t s_boot[DISK_BLOCK];
static uint8_t s_fat[FAT_SECTORS * DISK_BLOCK];
static uint8_t s_root[ROOT_SECTORS * DISK_BLOCK];

/*===========================================================================
 * 4KB 写回缓存
 *===========================================================================*/
#define WC_N        8u
#define FLASH_SECT  4096u

typedef struct {
    uint32_t blk;        /* flash 上的 4KB 块号（相对 App 数据区）；0xFFFFFFFF = 空 */
    uint8_t  dirty;
    uint8_t  buf[FLASH_SECT];
} wc_ent_t;

static wc_ent_t s_wc[WC_N];
static uint32_t s_wc_clock;

/* 统计 / 供上层判断进度 */
static volatile uint32_t s_stat_read_sect;
static volatile uint32_t s_stat_write_sect;
static volatile uint32_t s_stat_flash_erase;
static volatile uint32_t s_stat_flash_prog;
static volatile uint32_t s_data_hi;        /* 数据区被写到过的最高字节偏移 */

/* ---- 目录项解析出来的文件信息 ---- */
typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint32_t size;
    uint16_t first_clu;
    int      valid;
} file_info_t;

static file_info_t s_file;

/*===========================================================================
 * 小工具
 *===========================================================================*/
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mem_copy(void *d, const void *s, uint32_t n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) {
        *dd++ = *ss++;
    }
}
static void mem_set(void *d, int c, uint32_t n)
{
    uint8_t *dd = d;
    while (n--) {
        *dd++ = (uint8_t)c;
    }
}

/*===========================================================================
 * 引导扇区 / FAT 初始化（开机重建 => 永远是一张"空盘"）
 *===========================================================================*/
static void build_boot_sector(void)
{
    uint8_t *b = s_boot;

    mem_set(b, 0, DISK_BLOCK);
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;          /* jmp + nop */
    mem_copy(b + 3, "S31BOOT ", 8);                  /* OEM */
    wr16(b + 11, DISK_BLOCK);                        /* 每扇区字节数 */
    b[13] = CLU_SECTORS;                             /* 每簇扇区数 */
    wr16(b + 14, RESERVED_SECT);                     /* 保留扇区 */
    b[16] = 2;                                       /* FAT 份数 */
    wr16(b + 17, ROOT_ENTRIES);                      /* 根目录项数 */
    wr16(b + 19, (uint16_t)TOTAL_SECTORS);           /* 小卷：totSec16 */
    b[21] = 0xF8;                                    /* 媒体描述符：固定盘 */
    wr16(b + 22, FAT_SECTORS);                       /* 每份 FAT 扇区数 */
    wr16(b + 24, 32);                                /* 每道扇区 */
    wr16(b + 26, 64);                                /* 磁头数 */
    wr32(b + 28, 0);                                 /* 隐藏扇区 */
    wr32(b + 32, 0);                                 /* totSec32 = 0（用 totSec16）*/
    b[36] = 0x80;                                    /* 驱动器号 */
    b[38] = 0x29;                                    /* 扩展引导签名 */
    wr32(b + 39, 0x5333312Bu);                       /* 卷序列号 "S31+" */
    mem_copy(b + 43, "S31-BOOT   ", 11);             /* 卷标（资源管理器里显示这个）*/
    mem_copy(b + 54, "FAT16   ", 8);                 /* 文件系统类型字符串 */
    b[510] = 0x55;
    b[511] = 0xAA;
}

static void build_fat(void)
{
    mem_set(s_fat, 0, sizeof(s_fat));
    wr16(s_fat + 0, 0xFFF8);       /* 簇 0：媒体描述符 */
    wr16(s_fat + 2, 0xFFFF);       /* 簇 1：固定 EOC */
    /* 簇 2.. 全 0 = 空闲 */
}

/* 读 FAT 表项（簇号的下一簇；0xFFFF = EOC / 0 = 空闲）*/
static uint32_t fat_get(uint32_t clu)
{
    if (clu >= CLU_COUNT + 2u) {
        return 0xFFFFu;
    }
    return rd16(s_fat + clu * 2u);
}

static void build_root(void)
{
    uint8_t *e = s_root;

    mem_set(s_root, 0, sizeof(s_root));

    /* 第 0 项放**卷标项**（attr=0x08）：Windows 资源管理器里显示的卷名读的是这里，
     * 不是引导扇区里那个 BPB 卷标字段。（没有这一项时盘符后面是空的。）*/
    mem_copy(e, "S31-BOOT   ", 11);
    e[11] = 0x08;

    /* 之后是空的：不放任何文件 —— 这样 Windows 分配的第一个可用簇就是 2，
        拖进来的镜像落在数据区最前面（= S31_APP_LOAD_BASE，固定地址）。
        ★ 就算 Windows 先拿几个簇去做 System Volume Information 导致镜像
          首簇不是 2，msc_disk.c 的 data_map_off() 也会按"相对首簇"折算，
          镜像照样落在 S31_APP_LOAD_BASE。 */
}

/*===========================================================================
 * 4KB 写回缓存
 *===========================================================================*/
static void wc_flush(int i)
{
    wc_ent_t *e = &s_wc[i];
    uint32_t addr;

    if (!e->dirty || e->blk == 0xFFFFFFFFu) {
        return;
    }
    addr = S31_APP_DATA_BASE + e->blk * FLASH_SECT;
    s31_flash_erase_sector(addr);
    s_stat_flash_erase++;
    s31_flash_write(addr, e->buf, FLASH_SECT);
    s_stat_flash_prog++;
    e->dirty = 0;
}

static void wc_flush_all(void)
{
    uint32_t i;
    for (i = 0; i < WC_N; i++) {
        wc_flush(i);
    }
}

static wc_ent_t *wc_get(uint32_t blk)
{
    uint32_t i, lru = 0;
    wc_ent_t *e;

    /* 命中？ */
    for (i = 0; i < WC_N; i++) {
        if (s_wc[i].blk == blk) {
            return &s_wc[i];
        }
    }
    /* 找空位 */
    for (i = 0; i < WC_N; i++) {
        if (s_wc[i].blk == 0xFFFFFFFFu) {
            lru = i;
            goto fill;
        }
    }
    /* 没有空位：淘汰最久没碰过的（这里简单用轮转序） */
    lru = s_wc_clock % WC_N;
    wc_flush(lru);

fill:
    e = &s_wc[lru];
    e->blk = blk;
    e->dirty = 0;
    /* read-modify-write：先把旧内容读进来，保证"部分块写"也正确 */
    s31_flash_read(S31_APP_DATA_BASE + blk * FLASH_SECT, e->buf, FLASH_SECT);
    return e;
}

/* 往数据区写 off/len（off 相对 App 数据区） */
static void data_write(uint32_t off, const uint8_t *src, uint32_t len)
{
    while (len) {
        uint32_t blk = off / FLASH_SECT;
        uint32_t inblk = off % FLASH_SECT;
        uint32_t n = FLASH_SECT - inblk;
        wc_ent_t *e;

        if (n > len) {
            n = len;
        }
        e = wc_get(blk);
        mem_copy(e->buf + inblk, src, n);
        e->dirty = 1;
        s_wc_clock++;

        /* 整块写满 → 立刻刷（顺序写时这是常态，能让缓存不打架）*/
        if (n == FLASH_SECT) {
            wc_flush((int)(e - s_wc));
        }

        /* 统计：数据区被写到过的最高字节偏移（状态行里用来判断"拖到哪儿了"）*/
        if (off + n > s_data_hi) {
            s_data_hi = off + n;
        }

        off += n;
        src += n;
        len -= n;
    }
}

/*===========================================================================
 * 根目录解析：找出（唯一的）那个普通文件
 *===========================================================================*/
static void parse_root_dir(void)
{
    uint32_t i;

    s_file.valid = 0;
    s_file.size = 0;
    s_file.first_clu = 0;

    for (i = 0; i < ROOT_ENTRIES; i++) {
        uint8_t *e = s_root + i * 32;
        uint8_t attr = e[11];

        if (e[0] == 0x00) {
            break;                       /* 目录结束 */
        }
        if (e[0] == 0xE5) {
            continue;                    /* 已删除 */
        }
        if ((attr & 0x0F) == 0x0F) {
            continue;                    /* 长文件名项 */
        }
        if (attr & 0x08) {
            continue;                    /* 卷标 */
        }
        if (attr & 0x10) {
            continue;                    /* 目录 */
        }
        /* 普通文件 */
        mem_copy(s_file.name, e, 11);
        s_file.attr = attr;
        s_file.size = rd32(e + 28);
        s_file.first_clu = rd16(e + 26);
        s_file.valid = 1;
        return;
    }
}

/* 名字是不是 .BIN 结尾 */
static int name_is_bin(void)
{
    const uint8_t *n = s_file.name;
    return ((n[8] == 'B' || n[8] == 'b') &&
            (n[9] == 'I' || n[9] == 'i') &&
            (n[10] == 'N' || n[10] == 'n'));
}

/*===========================================================================
 * ★ 数据区映射：把"磁盘数据区偏移"折成"镜像内偏移"
 *
 * 目的：**不管 FAT 把文件分到了哪个簇，镜像都落在同一个固定 flash 地址**
 *       （S31_APP_LOAD_BASE = 0x100000）。这样 App 的 LMA 才能是链接期常量。
 *
 * 做法：按**相对于文件首簇**的偏移算，而不是相对数据区起点：
 *         镜像内偏移 = (簇号 - 首簇号) * 簇大小 + 簇内偏移
 *
 *   例：首簇 = 2  → 簇 2,3,4... 映到 0,1024,2048...（和绝对映射一样）
 *       首簇 = 37 → 簇 37,38,39… 也映到 0,1024,2048…（自动归一化）
 *
 * ⚠️ 这**要求文件在 flash 上连续**（簇号连续）。我们的卷是每次开机用 RAM 里
 *    的全空 FAT 重建的，Windows 从最低空闲簇开始分，单文件必然是连续的。
 *    msc_disk_file_contig() 会再验一遍；不连续的话镜像会被打乱，
 *    但 CRC/魔术字校验会立刻发现 -> 留在 bootloader，不会跑飞。
 *
 * ⚠️ 目录项是在**写数据之前**写的（Windows：建目录项 → 写数据），
 *    所以写数据时 s_file.first_clu 已经是新文件的首簇了。
 *    万一没有有效目录项，就退回绝对映射（等价于老行为）。
 *===========================================================================*/
static uint32_t data_map_off(uint32_t off)
{
    if (s_file.valid && s_file.first_clu >= CLU_FIRST) {
        uint32_t clu = CLU_FIRST + off / CLU_SIZE;

        if (clu >= s_file.first_clu) {
            uint32_t m = (clu - s_file.first_clu) * CLU_SIZE + (off % CLU_SIZE);

            if (m < DISK_DATA_BYTES) {
                return m;
            }
        }
        return 0xFFFFFFFFu;      /* 越界：这个扇区不属于镜像 */
    }
    return off;
}

/* 文件在磁盘数据区里的簇号是不是连续的（连续 => 镜像在 flash 里也是连续的）。
 * 返回：1 = 连续、0 = 不连续/读不到、-1 = 没有文件。 */
int msc_disk_file_contig(void)
{
    uint32_t clu, next, want, left;

    if (!s_file.valid || s_file.first_clu < CLU_FIRST) {
        return -1;
    }
    clu = s_file.first_clu;
    want = (s_file.size + CLU_SIZE - 1u) / CLU_SIZE;   /* 需要几个簇 */
    left = want;

    while (left > 0) {
        if (clu < CLU_FIRST || clu > CLU_LAST) {
            return 0;
        }
        next = fat_get(clu);
        left--;
        if (left == 0) {
            break;
        }
        if (next != clu + 1) {           /* 必须紧挨着 */
            return 0;
        }
        clu = next;
    }
    return 1;
}

/*===========================================================================
 * MSC 接口
 *===========================================================================*/
void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t *block_num, uint32_t *block_size)
{
    (void)busid;
    (void)lun;
    *block_num = TOTAL_SECTORS;
    *block_size = DISK_BLOCK;
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length)
{
    (void)busid;
    (void)lun;

    s_stat_read_sect++;

    if (sector == 0) {
        mem_copy(buffer, s_boot, DISK_BLOCK);
        return 0;
    }
    if (sector >= FAT1_LBA && sector < FAT1_LBA + FAT_SECTORS) {
        mem_copy(buffer, s_fat + (sector - FAT1_LBA) * DISK_BLOCK, DISK_BLOCK);
        return 0;
    }
    if (sector >= FAT2_LBA && sector < FAT2_LBA + FAT_SECTORS) {
        mem_copy(buffer, s_fat + (sector - FAT2_LBA) * DISK_BLOCK, DISK_BLOCK);
        return 0;
    }
    if (sector >= ROOT_LBA && sector < ROOT_LBA + ROOT_SECTORS) {
        mem_copy(buffer, s_root + (sector - ROOT_LBA) * DISK_BLOCK, DISK_BLOCK);
        return 0;
    }
    if (sector >= DATA_LBA && sector < DATA_LBA + DATA_SECTORS) {
        uint32_t off = (sector - DATA_LBA) * DISK_BLOCK;
        uint32_t n = length ? length : DISK_BLOCK;
        uint32_t m = data_map_off(off);          /* ★ 折成镜像内偏移 */

        if (off + n > DISK_DATA_BYTES) {
            n = DISK_DATA_BYTES - off;
        }
        if (m == 0xFFFFFFFFu) {
            mem_set(buffer, 0, DISK_BLOCK);
            return 0;
        }
        s31_flash_read(S31_APP_LOAD_BASE + m, buffer, n);
        return 0;
    }
    mem_set(buffer, 0, length ? length : DISK_BLOCK);
    return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length)
{
    (void)busid;
    (void)lun;

    s_stat_write_sect++;

    if (sector == 0) {
        return 0;                        /* 引导扇区只读（我们重建） */
    }
    if (sector >= FAT1_LBA && sector < FAT1_LBA + FAT_SECTORS) {
        mem_copy(s_fat + (sector - FAT1_LBA) * DISK_BLOCK, buffer, DISK_BLOCK);
        return 0;
    }
    if (sector >= FAT2_LBA && sector < FAT2_LBA + FAT_SECTORS) {
        mem_copy(s_fat + (sector - FAT2_LBA) * DISK_BLOCK, buffer, DISK_BLOCK);
        return 0;
    }
    if (sector >= ROOT_LBA && sector < ROOT_LBA + ROOT_SECTORS) {
        mem_copy(s_root + (sector - ROOT_LBA) * DISK_BLOCK, buffer, DISK_BLOCK);
        parse_root_dir();
        return 0;
    }
    if (sector >= DATA_LBA && sector < DATA_LBA + DATA_SECTORS) {
        uint32_t off = (sector - DATA_LBA) * DISK_BLOCK;
        uint32_t n = length ? length : DISK_BLOCK;
        uint32_t m = data_map_off(off);          /* ★ 折成镜像内偏移 */

        if (off + n > DISK_DATA_BYTES) {
            n = DISK_DATA_BYTES - off;
        }
        if (m == 0xFFFFFFFFu) {
            return 0;                            /* 越界：丢掉 */
        }
        data_write(m, buffer, n);
        return 0;
    }
    return -1;
}

/*===========================================================================
 * 对外
 *===========================================================================*/
void msc_disk_init(void)
{
    uint32_t i;

    for (i = 0; i < WC_N; i++) {
        s_wc[i].blk = 0xFFFFFFFFu;
        s_wc[i].dirty = 0;
    }
    s_data_hi = 0;
    s_file.valid = 0;

    build_boot_sector();
    build_fat();
    build_root();

    kprintf("[disk] FAT16 %u sectors (%u KB), cluster %u B x %u, data %u sectors @flash 0x%x\r\n",
            (unsigned)TOTAL_SECTORS, (unsigned)(TOTAL_SECTORS / 2),
            (unsigned)CLU_SIZE, (unsigned)CLU_COUNT,
            (unsigned)DATA_SECTORS, (unsigned)S31_APP_DATA_BASE);
}

uint32_t msc_disk_read_sect(void)  { return s_stat_read_sect; }
uint32_t msc_disk_write_sect(void) { return s_stat_write_sect; }
uint32_t msc_disk_erase_cnt(void)  { return s_stat_flash_erase; }
uint32_t msc_disk_prog_cnt(void)   { return s_stat_flash_prog; }
uint32_t msc_disk_data_hi(void)    { return s_data_hi; }

/* 把文件信息 / 完成状态暴露给主循环 */
int msc_disk_get_file(uint32_t *size, uint16_t *first_clu, int *is_bin)
{
    if (!s_file.valid) {
        return 0;
    }
    *size = s_file.size;
    *first_clu = s_file.first_clu;
    *is_bin = name_is_bin();
    return 1;
}

void msc_disk_flush(void)
{
    wc_flush_all();
}
