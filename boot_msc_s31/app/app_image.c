/*===========================================================================
 * app_image.c -- App 镜像的校验、描述符读写、搬运与跳转
 *
 * 镜像特征（详见 s31_layout.h）：
 *   - 存在 flash 的 App 数据区里，起点 = S31_APP_DATA_BASE + src_off
 *   - 开头是 s31_app_header_t：magic('S31A') + 段表 + total_size + crc32
 *   - crc32 覆盖 [0, total_size)，计算时 crc32 字段本身按 0 计
 *
 * 描述符块（flash S31_APP_DESC_OFFSET，独占 4KB）是 bootloader 判断
 * "应用地址有没有程序"的唯一依据：magic + desc_crc 对了才继续校验镜像。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"
#include "s31_layout.h"

int kprintf(const char *fmt, ...);
int s31_flash_read(uint32_t addr, void *dst, uint32_t len);
int s31_flash_write(uint32_t addr, const void *src, uint32_t len);
int s31_flash_erase_sector(uint32_t addr);
/* PSRAM 的"原始事务"读写（bsp/s31_psram.c）——搬运 PSRAM 段时用它：
   它不经过 cache，所以不怕 cache 里的陈旧行；自检也用它 + cache 读交叉验证。 */
void s31_psram_write_raw(uint32_t paddr, const void *buf, uint32_t len);
void s31_psram_invalidate_cache(uint32_t vaddr, uint32_t len);

/*===========================================================================
 * CRC32（标准 reflected，多项式 0xEDB88320）
 *===========================================================================*/
uint32_t s31_crc32(uint32_t crc, const uint8_t *p, uint32_t len)
{
    uint32_t i;
    int k;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/*===========================================================================
 * 描述符
 *===========================================================================*/
int app_desc_read(s31_appdesc_t *d)
{
    s31_flash_read(S31_APP_DESC_OFFSET, d, sizeof(*d));
    return 0;
}

int app_desc_write(const s31_appdesc_t *d)
{
    s31_appdesc_t tmp = *d;
    uint32_t crc;

    /* desc_crc 覆盖前面 15 个字（desc_crc 自己除外）*/
    tmp.desc_crc = 0;
    crc = s31_crc32(0, (const uint8_t *)&tmp, sizeof(tmp) - 4);
    tmp.desc_crc = crc;

    s31_flash_erase_sector(S31_APP_DESC_OFFSET);
    return s31_flash_write(S31_APP_DESC_OFFSET, &tmp, sizeof(tmp));
}

int app_desc_valid(const s31_appdesc_t *d)
{
    s31_appdesc_t tmp;
    uint32_t crc;

    if (d->magic != S31_APPDESC_MAGIC) {
        return 0;
    }
    tmp = *d;
    tmp.desc_crc = 0;
    crc = s31_crc32(0, (const uint8_t *)&tmp, sizeof(tmp) - 4);
    return (crc == d->desc_crc);
}

/*===========================================================================
 * 镜像校验：头合法性 + 全镜像 CRC32
 *
 * ★ 支持两种格式（详见 bsp/s31_layout.h 的说明）：
 *   A) 开头 4 字节 == 'S31A' -> 带段表的容器（tools/mkapp.py 产出）
 *   B) 其它一律当作**扁平的 objcopy 产物**：
 *        单个"段"：整份文件从 S31_APP_FLAT_VMA 开始装，入口也是那里。
 *        长度必须由调用方给（file_size）—— 扁平镜像里没有长度字段。
 *
 * 无论哪种格式，返回时 out_hdr 都是**填好的、统一的结构**，
 * 下游的 app_image_load()/入口跳转都不用关心是哪种。
 *
 * 返回 0 = OK；负数 = 具体原因
 *===========================================================================*/
#define IMG_CHUNK 4096u
static uint8_t s_chunk[IMG_CHUNK];

/* 把"扁平镜像"包装成一个单段头（放在调用方给的 buffer 里，别用 static：
   万一将来重入会踩）*/
static void flat_to_header(uint32_t file_size, s31_app_header_t *h)
{
    uint32_t i;

    for (i = 0; i < sizeof(*h) / 4u; i++) {
        ((uint32_t *)h)[i] = 0;
    }
    h->magic = S31_APP_MAGIC;
    h->version = 1;
    h->entry = S31_APP_FLAT_VMA;
    h->nsegs = 1;
    h->total_size = file_size;
    h->segs[0].off = 0;                     /* 从文件第 0 字节开始搬 */
    h->segs[0].vma = S31_APP_FLAT_VMA;
    h->segs[0].size = file_size;
    h->segs[0].flags = 0;
}

/* flash_addr 是镜像在 flash 里的**绝对地址**（= S31_APP_LOAD_BASE，拖拽时固定写到那儿）。
 * ⚠️ 以前这个参数是"相对 App 数据区的偏移"，函数内部再加 S31_APP_DATA_BASE；
 *    现在加载地址本身就是 S31_APP_DATA_BASE，再加一次就变成 0x200000 了 —— 踩过。 */
int app_image_check(uint32_t flash_addr, uint32_t file_size,
                    s31_app_header_t *out_hdr, uint32_t *out_crc)
{
    s31_app_header_t h;
    uint32_t base = flash_addr;
    uint32_t off = flash_addr - S31_APP_DATA_BASE;     /* 相对数据区，只用来做范围检查 */
    uint32_t crc, done;
    uint32_t i;
    uint32_t hdr_len;                       /* CRC 时要跳过的头部长度 */
    int is_flat;

    if (flash_addr < S31_APP_DATA_BASE || off + sizeof(h) > S31_APP_DATA_SIZE) {
        return -1;
    }
    s31_flash_read(base, &h, sizeof(h));

    is_flat = (h.magic != S31_APP_MAGIC);

    if (is_flat) {
        /* ---- 格式 B：扁平 objcopy 产物 ---- */
        if (file_size < 16u || file_size > S31_APP_DATA_SIZE ||
            off + file_size > S31_APP_DATA_SIZE) {
            kprintf("[img] 扁平镜像大小不对: %u (off=%u)\r\n",
                    (unsigned)file_size, (unsigned)off);
            return -2;
        }
        flat_to_header(file_size, &h);
        hdr_len = 0;                        /* 扁平镜像没有头，整个文件都算数据 */
        kprintf("[img] 未识别到 'S31A' 段表 -> 按**扁平镜像**处理: %u B -> 0x%08x\r\n",
                (unsigned)file_size, (unsigned)S31_APP_FLAT_VMA);
    } else {
        /* ---- 格式 A：带段表的容器 ---- */
        hdr_len = 64u;
        if (h.version != 1u) {
            kprintf("[img] bad version %u\r\n", (unsigned)h.version);
            return -3;
        }
        if (h.nsegs == 0 || h.nsegs > S31_APP_MAX_SEGS) {
            kprintf("[img] bad nsegs %u\r\n", (unsigned)h.nsegs);
            return -4;
        }
        if (h.total_size < sizeof(h) || h.total_size > S31_APP_DATA_SIZE ||
            off + h.total_size > S31_APP_DATA_SIZE) {
            kprintf("[img] bad total_size %u (off=%u)\r\n",
                    (unsigned)h.total_size, (unsigned)off);
            return -5;
        }
        if (file_size != 0 && file_size < h.total_size) {
            kprintf("[img] 文件比头里声明的短: file=%u hdr=%u\r\n",
                    (unsigned)file_size, (unsigned)h.total_size);
            return -5;
        }

        /* 段表自检：地址必须落在 PSRAM 或 App 可用的内部 RAM 区间 */
        for (i = 0; i < h.nsegs; i++) {
            const s31_app_seg_t *s = &h.segs[i];
            int in_psram = (s->vma >= S31_PSRAM_VADDR &&
                            s->vma + s->size <= S31_PSRAM_VADDR + S31_PSRAM_VSIZE);
            int in_iram = (s->vma >= S31_APP_IRAM_BASE &&
                           s->vma + s->size <= S31_RAM_HIGH);
            if (!in_psram && !in_iram) {
                kprintf("[img] seg%u vma 0x%08x size %u out of range\r\n",
                        (unsigned)i, (unsigned)s->vma, (unsigned)s->size);
                return -6;
            }
            if (!(s->flags & 1u) && (s->off + s->size > h.total_size)) {
                kprintf("[img] seg%u off/size beyond image\r\n", (unsigned)i);
                return -7;
            }
        }
        if (h.entry < S31_PSRAM_VADDR + 0x10u && h.entry < S31_APP_IRAM_BASE) {
            kprintf("[img] entry 0x%08x out of range\r\n", (unsigned)h.entry);
            return -8;
        }
    }

    /* 全镜像 CRC32。
     * 容器格式：跳过 64 字节头（hdr_len），且 crc32 字段本身按 0 计（和打包工具一致）。
     * 扁平格式：整个文件从头算（hdr_len = 0），没有任何字段要清零。 */
    if (is_flat) {
        crc = 0;
        done = 0;
    } else {
        uint32_t hdr_copy[16];               /* sizeof(s31_app_header_t) == 64 */
        uint32_t *hp = (uint32_t *)&h;

        for (i = 0; i < 16; i++) {
            hdr_copy[i] = hp[i];
        }
        hdr_copy[5] = 0;                     /* crc32 字段按 0 计 */
        crc = s31_crc32(0, (const uint8_t *)hdr_copy, hdr_len);
        done = hdr_len;
    }
    while (done < h.total_size) {
        uint32_t n = h.total_size - done;

        if (n > IMG_CHUNK) {
            n = IMG_CHUNK;
        }
        s31_flash_read(base + done, s_chunk, n);
        crc = s31_crc32(crc, s_chunk, n);
        done += n;
    }

    if (out_hdr) {
        *out_hdr = h;
    }
    if (out_crc) {
        *out_crc = crc;
    }

    /* 容器格式：头里存了 crc32，可以就地比对。
     * 扁平格式：**头里没地方存 CRC**，所以这里只负责算出来（通过 out_crc 交给调用方），
     *          完整性由"描述符里存的 img_crc"在开机时比对 —— 拖拽当场没有参照物可比，
     *          但数据是刚写进 flash 又刚读回来的，这一步算一次就够了。 */
    if (!is_flat && crc != h.crc32) {
        kprintf("[img] CRC mismatch: calc %08x, header %08x\r\n",
                (unsigned)crc, (unsigned)h.crc32);
        return -9;
    }
    return 0;
}

/*===========================================================================
 * 搬运段 + 跳转
 *
 * 段表给出"从 flash 哪里搬到内存哪里"：
 *   - LMA = flash_addr + seg.off（flash_addr 是**绝对**地址 = S31_APP_LOAD_BASE）
 *   - VMA = seg.vma（PSRAM 0x50000000 或内部 RAM [0x2F060000, 0x2F07AFC0)）
 * flags bit0 = 1 表示"只清零不搬"（bss）
 *
 * ⚠️ PSRAM 段只有在 s31_psram_init() 成功之后才允许搬 —— 否则一写就
 *    store access fault。调用方负责判断（app_image_load 会再挡一道）。
 *===========================================================================*/
#define LOAD_CHUNK 4096u
static uint8_t s_loadbuf[LOAD_CHUNK];

int app_image_load(const s31_app_header_t *h, uint32_t flash_addr, int psram_ok)
{
    uint32_t base = flash_addr;
    uint32_t i;

    for (i = 0; i < h->nsegs; i++) {
        const s31_app_seg_t *s = &h->segs[i];
        uint32_t done = 0;
        int is_psram = (s->vma >= S31_PSRAM_VADDR &&
                        s->vma < S31_PSRAM_VADDR + S31_PSRAM_VSIZE);

        if (is_psram && !psram_ok) {
            kprintf("[app] seg%u 要放 PSRAM 但 PSRAM 不可用 -> 放弃\r\n", (unsigned)i);
            return -2;
        }

        if (s->flags & 1u) {
            /* bss：清零 */
            uint32_t off = 0;

            if (is_psram) {
                /* ★ PSRAM 的零不能靠普通 store（cache 写外部存储 = access fault），
                   走原始 MSPI 事务。 */
                static const uint8_t zero64[64] = { 0 };
                while (off < s->size) {
                    uint32_t n = s->size - off;
                    if (n > sizeof(zero64)) {
                        n = sizeof(zero64);
                    }
                    s31_psram_write_raw(s->vma - S31_PSRAM_VADDR + off, zero64, n);
                    off += n;
                }
                s31_psram_invalidate_cache(s->vma, s->size);
            } else {
                uint8_t *d = (uint8_t *)(uintptr_t)s->vma;
                uint32_t n = s->size;
                while (n--) {
                    *d++ = 0;
                }
            }
            kprintf("[app] seg%u: zero %u B @0x%08x%s\r\n",
                    (unsigned)i, (unsigned)s->size, (unsigned)s->vma,
                    is_psram ? " (raw)" : "");
            continue;
        }

        while (done < s->size) {
            uint32_t n = s->size - done;
            if (n > LOAD_CHUNK) {
                n = LOAD_CHUNK;
            }
            s31_flash_read(base + s->off + done, s_loadbuf, n);
            if (is_psram) {
                /* ★ 同上：PSRAM 段走原始事务；写完让 cache 里的旧行失效，
                   否则 App 启动后 cache 读会拿到陈旧内容。 */
                s31_psram_write_raw(s->vma - S31_PSRAM_VADDR + done, s_loadbuf, n);
            } else {
                uint8_t *d = (uint8_t *)(uintptr_t)(s->vma + done);
                const uint8_t *sp = s_loadbuf;
                uint32_t k = n;
                while (k--) {
                    *d++ = *sp++;
                }
            }
            done += n;
        }
        if (is_psram) {
            s31_psram_invalidate_cache(s->vma, s->size);
        }
        kprintf("[app] seg%u: copy %u B flash+%u -> 0x%08x%s\r\n",
                (unsigned)i, (unsigned)s->size, (unsigned)s->off, (unsigned)s->vma,
                is_psram ? " (raw)" : "");
    }
    return 0;
}

/* 跳进 App：
 *   - 关中断（App 自己会重新配）
 *   - 清 mscratch（我们的 ISR 栈约定只对 bootloader 有意义）
 *   - mtvec 先清成 0：App 的 startup 会自己设
 *   - 跳到 entry（App 的 _start 第一件事就是重设 sp）
 * 注意：**不返回**。 */
void app_jump(uint32_t entry) __attribute__((noreturn));

void app_jump(uint32_t entry)
{
    kprintf("[app] jumping to 0x%08x ...\r\n", (unsigned)entry);

    __asm__ volatile ("csrci mstatus, 0x8");      /* MIE = 0 */
    __asm__ volatile ("csrw mscratch, zero");
    __asm__ volatile ("csrw mtvec, zero");
    __asm__ volatile ("fence.i");

    ((void (*)(void))(uintptr_t)entry)();

    for (;;) {
        /* 不该到这儿 */
    }
}
