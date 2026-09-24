/*===========================================================================
 * mini_libc.c -- -nostdlib 下的最小 libc：字符串/内存 + 自写 kprintf
 *
 * 输出走 USB-Serial/JTAG（s31_usj.c），不是 UART0（那块没接线）。
 * 不链 newlib：控制得住、体积小、没有 syscall 桩的麻烦。
 *===========================================================================*/
#include <stddef.h>
#include <stdint.h>

void s31_usj_putc(char c);
void s31_usj_pump(void);

/*===========================================================================
 * printf
 *
 * 🚨 变参位宽必须和调用方**实际传的类型**对齐（RISC-V ilp32 ABI）：
 *    传 `unsigned`（4 字节）就占 1 个寄存器，按 8 字节取会把后面所有参数
 *    都读错位 —— 实测症状是 %s 拿到野指针 0x2F，直接 Load access fault。
 *    所以：%d/%u/%x/%X/%p/%c 一律按 32 位（long/unsigned long）取，
 *    只有写了 `ll` 才按 64 位取。
 *===========================================================================*/
static void put_u64(unsigned long long v, unsigned base, int upper, int width, char pad)
{
    char buf[24];
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        buf[i++] = '0';
    }
    while (v) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (width > i) {
        s31_usj_putc(pad);
        width--;
    }
    while (i) {
        s31_usj_putc(buf[--i]);
    }
}

int vkprintf(const char *fmt, __builtin_va_list ap)
{
    const char *p = fmt;
    char c;

    while ((c = *p++) != 0) {
        int width = 0;
        char pad = ' ';
        int is64 = 0;

        if (c != '%') {
            if (c == '\n') {
                s31_usj_putc('\r');       /* 终端要 CRLF */
            }
            s31_usj_putc(c);
            continue;
        }
        c = *p++;
        if (c == '0') {
            pad = '0';
            c = *p++;
        }
        while (c >= '0' && c <= '9') {
            width = width * 10 + (c - '0');
            c = *p++;
        }
        if (c == 'l') {                   /* 32 位；再跟一个 l 才是 64 位 */
            c = *p++;
            if (c == 'l') {
                is64 = 1;
                c = *p++;
            }
        }
        switch (c) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (*s) {
                s31_usj_putc(*s++);
            }
            break;
        }
        case 'c':
            s31_usj_putc((char)__builtin_va_arg(ap, int));
            break;
        case 'd':
        case 'i':
            if (is64) {
                long long v = __builtin_va_arg(ap, long long);
                if (v < 0) {
                    s31_usj_putc('-');
                    put_u64((unsigned long long)(-v), 10, 0, 0, ' ');
                } else {
                    put_u64((unsigned long long)v, 10, 0, 0, ' ');
                }
            } else {
                int v = __builtin_va_arg(ap, int);
                if (v < 0) {
                    s31_usj_putc('-');
                    put_u64((unsigned long long)(unsigned)(-v), 10, 0, 0, ' ');
                } else {
                    put_u64((unsigned long long)(unsigned)v, 10, 0, 0, ' ');
                }
            }
            break;
        case 'u':
        case 'x':
        case 'X': {
            unsigned upper = (c == 'X');
            unsigned base = (c == 'u') ? 10u : 16u;
            if (is64) {
                put_u64(__builtin_va_arg(ap, unsigned long long), base, (int)upper, width, pad);
            } else {
                put_u64((unsigned long long)__builtin_va_arg(ap, unsigned long),
                        base, (int)upper, width, pad);
            }
            break;
        }
        case 'p':
            put_u64((unsigned long long)(uintptr_t)__builtin_va_arg(ap, void *), 16, 0, 8, '0');
            break;
        case '%': s31_usj_putc('%'); break;
        default:  s31_usj_putc('?'); break;
        }
    }
    s31_usj_pump();                       /* 整行一次性交给主机 */
    return 0;
}

/* 变参入口。拆出 vkprintf 是为了让 esp_rom_printf（IDF 的日志出口）也能复用同一套
   格式化 —— 没有它，IDF 那些 PSRAM 日志（vendor id / read latency / density）
   一条都看不到，排查时等于蒙眼。 */
int kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    int r;

    __builtin_va_start(ap, fmt);
    r = vkprintf(fmt, ap);
    __builtin_va_end(ap);
    return r;
}

/* IDF 的 hal/assert.h 在 HAL_ASSERT 失败时调它（见 bsp/sdkconfig.h 的断言级别=1）。
   给个"打印一行 + 停机"，比 __builtin_unreachable() 的未定义行为好排查得多。 */
__attribute__((noreturn)) void abort(void)
{
    kprintf("\r\n!! HAL_ASSERT 失败 -> 停机\r\n");
    for (;;) {
        s31_usj_pump();
    }
}

/*===========================================================================
 * 字符串 / 内存
 *===========================================================================*/
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    uint32_t *dw;
    const uint32_t *sw;

    while (n && ((uintptr_t)d & 3UL)) {
        *d++ = *s++;
        n--;
    }
    dw = (uint32_t *)d;
    sw = (const uint32_t *)s;
    while (n >= 4) {
        *dw++ = *sw++;
        n -= 4;
    }
    d = (uint8_t *)dw;
    s = (const uint8_t *)sw;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = s;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i]; i++) {
        d[i] = s[i];
    }
    for (; i < n; i++) {
        d[i] = 0;
    }
    return d;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    while (n--) {
        if (*p == (uint8_t)c) {
            return (void *)p;
        }
        p++;
    }
    return 0;
}

void bzero(void *s, size_t n) { memset(s, 0, n); }

/*===========================================================================
 * 64 位除法 / 取模：-nostdlib 下 gcc 会引用 libgcc 的 __udivdi3/__umoddi3
 *===========================================================================*/
unsigned long long __udivdi3(unsigned long long n, unsigned long long d)
{
    unsigned long long q = 0, r = 0;
    int i, started = 0;

    if (d == 0) {
        return 0;
    }
    for (i = 63; i >= 0; i--) {
        unsigned long long bit = (n >> i) & 1ULL;
        if (!started && !bit && r == 0) {
            continue;
        }
        started = 1;
        r = (r << 1) | bit;
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }
    return q;
}

unsigned long long __umoddi3(unsigned long long n, unsigned long long d)
{
    unsigned long long q = 0, r = 0;
    int i, started = 0;

    if (d == 0) {
        return 0;
    }
    for (i = 63; i >= 0; i--) {
        unsigned long long bit = (n >> i) & 1ULL;
        if (!started && !bit && r == 0) {
            continue;
        }
        started = 1;
        r = (r << 1) | bit;
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }
    (void)q;
    return r;
}

long long __divdi3(long long n, long long d)
{
    int neg = 0;
    unsigned long long un, ud;
    if (n < 0) { un = (unsigned long long)(-n); neg ^= 1; } else { un = (unsigned long long)n; }
    if (d < 0) { ud = (unsigned long long)(-d); neg ^= 1; } else { ud = (unsigned long long)d; }
    {
        unsigned long long q = __udivdi3(un, ud);
        return neg ? -(long long)q : (long long)q;
    }
}

long long __moddi3(long long n, long long d)
{
    int neg = (n < 0);
    unsigned long long un = neg ? (unsigned long long)(-n) : (unsigned long long)n;
    unsigned long long ud = (d < 0) ? (unsigned long long)(-d) : (unsigned long long)d;
    unsigned long long r = __umoddi3(un, ud);
    return neg ? -(long long)r : (long long)r;
}
