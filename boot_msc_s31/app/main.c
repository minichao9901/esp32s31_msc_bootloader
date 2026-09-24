/*===========================================================================
 * main.c -- ESP32-S31 裸机 MSC bootloader
 *
 * 上电/复位时：
 *   ① 按住模式键（GPIO0 对 GND）        -> BOOTLOADER 模式
 *   ② 不按键 + 描述符有效 + 镜像校验通过 -> 跳进 App（M4）
 *   ③ 其它情况（没程序 / 校验失败）      -> 还是 BOOTLOADER 模式
 *
 * BOOTLOADER 模式：USB-HS 口枚举成一个 FAT16 小盘（8MB，卷标 S31-BOOT）。
 *   把 app.bin 拖进去 -> 数据**直接落进 flash 的 App 装载区**（固定 0x100000，
 *   见 msc_disk.c 的 data_map_off）-> 写完静默 1.5 秒 -> 校验镜像 -> 写描述符
 *   -> 自动复位进 App。
 *
 * ★ 本文件里那一大堆 `extern` 声明是**故意的**：裸机工程没有统一头文件，
 *   每个 .c 自己声明要用到的东西，改签名时靠编译器在这里报错。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"
#include "s31_layout.h"

#include "usbd_core.h"
#include "usbd_msc.h"

/* ---- bsp/ ---- */
int  kprintf(const char *fmt, ...);
void s31_usj_hw_init(void);
void s31_usj_pump(void);
void s31_systimer_init(void);
void s31_delay_ms(uint32_t ms);
uint32_t s31_millis(void);
void s31_system_reset(void);
void s31_flash_init(void);
int  s31_flash_read(uint32_t addr, void *dst, uint32_t len);
int  s31_flash_mmap_window(uint32_t flash_off, uint32_t len);
void s31_flash_mmap_invalidate(uint32_t flash_off, uint32_t len);
int  s31_psram_init(void);
uint32_t s31_psram_crc_test(uint32_t base, uint32_t bytes);
uint32_t s31_cpu_mhz(void);          /* 量真实 CPU 主频 */
int  s31_cpu_clk_boost(void);        /* 40MHz -> 320MHz（CPLL）*/
void s31_usb_interrupt_enable(void);
void s31_usb_soft_reconnect(void);   /* USB 软拔插：救"主机把枚举失败缓存住" */
void s31_usb_phy_off(void);          /* 跳 App 前关掉 USB PHY，别给主机留僵尸设备 */
uint32_t s31_usb_gintsts(void);      /* DWC2 中断状态（诊断用）*/
uint32_t s31_usb_dsts(void);         /* DWC2 设备状态（诊断用）*/
extern volatile uint32_t g_usb_irq_cnt;   /* USB ISR 进过几次 */
int  g_psram_ok;                     /* PSRAM 是否可用（决定要不要允许 App 段落到 PSRAM）*/

/* ---- app/ ---- */
void msc_disk_init(void);
void msc_disk_flush(void);
int  msc_disk_get_file(uint32_t *size, uint16_t *first_clu, int *is_bin);
int  msc_disk_file_contig(void);
uint32_t msc_disk_read_sect(void);
uint32_t msc_disk_write_sect(void);
uint32_t msc_disk_erase_cnt(void);
uint32_t msc_disk_prog_cnt(void);
uint32_t msc_disk_data_hi(void);
uint32_t s31_crc32(uint32_t crc, const uint8_t *p, uint32_t len);
int  app_desc_read(s31_appdesc_t *d);
int  app_desc_write(const s31_appdesc_t *d);
int  app_desc_valid(const s31_appdesc_t *d);
int  app_image_check(uint32_t flash_addr, uint32_t file_size, s31_app_header_t *out_hdr, uint32_t *out_crc);
int  app_image_load(const s31_app_header_t *h, uint32_t flash_addr, int psram_ok);
void app_jump(uint32_t entry) __attribute__((noreturn));

/*===========================================================================
 * 配置
 *===========================================================================*/
#define KEY_GPIO_BOOTLOADER   0        /* GPIO0 = J2 第 9 脚，低有效，内部上拉 */
#define DOWNLOAD_IDLE_MS      1500u    /* 写完静默多久算"拖完了" */
/* 一直没被主机配置时，隔多久做一次 USB 软拔插（见 s31_usb_soft_reconnect 的注释）。
   ⚠️ 必须**明显大于"主机正常枚举所需时间"**：实测 Windows 从设备挂上到 cfg=1
   最慢见过 ~5 秒，如果这里设 4 秒就会在正常枚举到一半时把设备拔掉 —— 自己把自己打断。*/
#define RECONNECT_EVERY_MS    10000u

/* USB 身份：303A:101A（工作区 PID 分配见 AGENTS §4.4：1018=cdc 1019=uac）*/
#define USBD_VID        0x303A
#define USBD_PID        0x101A
#define USBD_MAX_POWER  100

#define MSC_IN_EP       0x81
#define MSC_OUT_EP      0x01
#define MSC_FS_MAX_MPS  64
#define MSC_HS_MAX_MPS  512
#define USB_CONFIG_SIZE (9 + MSC_DESCRIPTOR_LEN)

/*===========================================================================
 * 描述符
 *===========================================================================*/
static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, MSC_FS_MAX_MPS, 0x02)
};

static const uint8_t config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, MSC_HS_MAX_MPS, 0x02)
};

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },     /* Langid = 0x0409 */
    "Espressif",
    "S31 MSC Bootloader",
    "S31-BOOT-0001",
    "S31 Bootloader MSC",
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    return (speed >= USB_SPEED_HIGH) ? config_descriptor_hs : config_descriptor_fs;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index > 4) {
        return NULL;
    }
    return string_descriptors[index];
}

static const struct usb_descriptor msc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

/*===========================================================================
 * USB 事件（中断上下文：只记状态，不打印）
 *===========================================================================*/
volatile uint32_t g_usb_events;
#define EVT_RESET      0x1u
#define EVT_CONNECTED  0x2u
#define EVT_CONFIGURED 0x4u

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
    case USBD_EVENT_RESET:
        g_usb_events |= EVT_RESET;
        g_usb_events &= ~EVT_CONFIGURED;
        break;
    case USBD_EVENT_CONNECTED:
        g_usb_events |= EVT_CONNECTED;
        break;
    case USBD_EVENT_DISCONNECTED:
        g_usb_events &= ~(EVT_CONNECTED | EVT_CONFIGURED);
        break;
    case USBD_EVENT_CONFIGURED:
        g_usb_events |= EVT_CONFIGURED;
        break;
    default:
        break;
    }
}

struct usbd_interface intf0;

/*===========================================================================
 * GPIO
 *===========================================================================*/
static void pin_input_pullup(int n)
{
    uint32_t v = S31_REG32(S31_IOMUX_REG(n));

    v &= ~((0x7u << S31_IOMUX_MCU_SEL_S) | S31_IOMUX_FUN_IE | S31_IOMUX_FUN_PU | S31_IOMUX_FUN_PD);
    v |= (S31_IOMUX_FUNC_GPIO << S31_IOMUX_MCU_SEL_S) | S31_IOMUX_FUN_IE | S31_IOMUX_FUN_PU;
    S31_REG32(S31_IOMUX_REG(n)) = v;

    if (n < 32) {
        S31_REG32(S31_GPIO_ENABLE_W1TC) = S31_GPIO_MASK(n);
    } else {
        S31_REG32(S31_GPIO_ENABLE1_W1TC) = S31_GPIO_MASK(n);
    }
}

static int pin_read(int n)
{
    uint32_t r = (n < 32) ? S31_REG32(S31_GPIO_IN_REG) : S31_REG32(S31_GPIO_IN1_REG);
    return (r & S31_GPIO_MASK(n)) ? 1 : 0;
}

/* 模式键去抖：连续 8 次都读到按下才算 */
static int key_pressed(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (pin_read(KEY_GPIO_BOOTLOADER) != 0) {
            return 0;
        }
        s31_delay_ms(1);
    }
    return 1;
}

/*===========================================================================
 * 复位原因
 *===========================================================================*/
static const char *rst_cause_str(uint32_t c)
{
    switch (c) {
    case 0x01: return "POR(power-on)";
    case 0x03: return "digital system reset";
    case 0x05: return "PMU power-down";
    case 0x07: return "HP WDT0";
    case 0x08: return "HP WDT1";
    case 0x0c: return "HP core soft reset";
    case 0x0f: return "brown-out";
    case 0x10: return "LP WDT (chip)";
    case 0x12: return "super watchdog";
    case 0x16: return "USB-JTAG request";
    case 0x17: return "USB-UART(CDC) request";
    case 0x18: return "JTAG";
    case 0x1a: return "HP core lockup";
    default:   return "other";
    }
}

static void reset_cause_report(void)
{
    uint32_t c0 = S31_REG32(S31_HPCORE0_RST_CAUSE);

    kprintf("[rst] core0=0x%02x (%s)\r\n",
            (unsigned)((c0 >> 1) & 0x3Fu), rst_cause_str((c0 >> 1) & 0x3Fu));
    S31_REG32(S31_HPCORE0_RST_CAUSE) = S31_RST_CAUSE_CLR;
}

/*===========================================================================
 * 启动决策：有没有可跑的 App？
 *===========================================================================*/
static s31_appdesc_t s_desc;

/* 返回 1 = 有可跑的应用（同时把描述符填好）*/
static int app_probe(void)
{
    s31_app_header_t hdr;
    uint32_t crc;
    int rc;

    app_desc_read(&s_desc);

    if (!app_desc_valid(&s_desc)) {
        kprintf("[app] descriptor invalid (magic=%08x state=%u) -> no App\r\n",
                (unsigned)s_desc.magic, (unsigned)s_desc.state);
        return 0;
    }
    if (s_desc.state != S31_APPDESC_STATE_READY) {
        kprintf("[app] descriptor state=%u -> no App\r\n", (unsigned)s_desc.state);
        return 0;
    }

    kprintf("[app] descriptor OK: size=%u crc=%08x entry=%08x\r\n",
            (unsigned)s_desc.img_size, (unsigned)s_desc.img_crc, (unsigned)s_desc.entry);

    /* ★ 镜像**固定**在 S31_APP_LOAD_BASE（拖拽时就写到那儿了，见 msc_disk.c 的
     *   data_map_off）—— 所以这里不再用 s_desc.src_off 去找镜像。
     *   这也是 App 能把 LMA 定死成 S31_APP_LMA_BASE 的原因。 */
    rc = app_image_check(S31_APP_LOAD_BASE, s_desc.img_size, &hdr, &crc);
    if (rc != 0) {
        kprintf("[app] image check FAILED (rc=%d) -> stay in BOOTLOADER\r\n", rc);
        return 0;
    }

    /* 🚨 **CRC 一定要在这里比对**（别只在拖拽时算一遍就完事）：
     *   容器格式的头里自带 crc32，app_image_check 已经比过了；
     *   但**扁平格式没有任何元数据** —— 光看"长度对、开头不是 'S31A'"是拦不住
     *   任何东西的：描述符说 READY、而 flash 0x100000 里其实是别的内容
     *   （拖失败残留 / 上一次的 FAT 目录项 / 擦到一半），照样会被当成合法镜像
     *   装进 PSRAM 并跳过去 —— 现象是**开机横幅之后串口就没声了**。
     *   本人在排查"经典模型"那轮就被它带偏过，最后靠 JTAG 抓
     *   `PC=0x00000000` + `ra` 落在 app_jump 才定位到。 */
    if (crc != s_desc.img_crc) {
        kprintf("[app] image CRC mismatch: calc %08x, descriptor %08x -> stay in BOOTLOADER\r\n",
                (unsigned)crc, (unsigned)s_desc.img_crc);
        return 0;
    }
    kprintf("[app] image OK: %u bytes, crc=%08x, %u segs, entry=0x%08x\r\n",
            (unsigned)hdr.total_size, (unsigned)crc,
            (unsigned)hdr.nsegs, (unsigned)hdr.entry);

    /* ★ 跳进 App 之前必须把 USB PHY 干净地关掉再交棒。
       否则：PHY 的上电位在常电域（HP_ALIVE / LP_SYS）跨复位保留，而 App 根本不碰 USB
       -> 主机一直看到一个不回答控制传输的僵尸设备 -> 判「设备描述符无效」并缓存住
       -> 之后怎么复位都识别不了，只有长时间断开（重新烧录/拔线）才恢复。
       实测完全吻合：纯 bootloader 反复复位 4/4 正常，跑过 App 再回来就废。 */
    kprintf("[app] 关掉 USB PHY（避免给主机留僵尸设备），然后跳转\r\n");
    s31_usb_phy_off();

    /* 校验过了就搬 + 跳 */
    if (app_image_load(&hdr, S31_APP_LOAD_BASE, g_psram_ok) != 0) {
        kprintf("[app] load FAILED -> stay in BOOTLOADER\r\n");
        return 0;
    }
    app_jump(hdr.entry);
    return 1;   /* 不会到这儿 */
}

/*===========================================================================
 * 拖拽收尾：校验 + 写描述符 + 复位
 *===========================================================================*/
static void download_finalize(void)
{
    uint32_t fsize = 0, crc = 0;
    uint16_t first_clu = 0;
    int is_bin = 0;
    s31_app_header_t hdr;
    s31_appdesc_t d;
    int rc;

    kprintf("[dl] quiet for %u ms -> finalizing\r\n", (unsigned)DOWNLOAD_IDLE_MS);

    if (!msc_disk_get_file(&fsize, &first_clu, &is_bin)) {
        kprintf("[dl] no file on the volume -> nothing to do\r\n");
        return;
    }
    kprintf("[dl] file: size=%u first_clu=%u bin=%d\r\n",
            (unsigned)fsize, (unsigned)first_clu, is_bin);

    if (first_clu < 2) {
        kprintf("[dl] bad first cluster %u\r\n", (unsigned)first_clu);
        return;
    }

    /* 先把 4KB 写回缓存里的脏块全部落盘，再校验 flash 里的内容 */
    kprintf("[dl] flushing write cache...\r\n");
    msc_disk_flush();
    /* 🚨 这行别删：状态行的 flash(erase/prog) 是**每 5 秒**打的，finalize 之后紧接着就复位，
       所以从状态行上看永远是 0 —— 我本人就被它误导过一轮，以为数据没落盘。*/
    kprintf("[dl] flush done: flash(erase=%u prog=%u)\r\n",
            (unsigned)msc_disk_erase_cnt(), (unsigned)msc_disk_prog_cnt());

    /* ★ 镜像固定落在 S31_APP_LOAD_BASE —— data_map_off() 已经把"文件首簇"折算掉了，
     *   所以首簇是不是 2 都不影响。这里只用 first_clu 打日志。
     *   先把映射窗口的 cache 作废：镜像刚用 SPI1 写进去，窗口里可能有旧行。*/
    s31_flash_mmap_invalidate(S31_APP_LOAD_BASE, 0x10000u);

    /* 连续性是"镜像落在固定地址"的前提：不连续的话 data_map_off 会把内容打乱，
     * 校验一定失败 -> 不会跑飞，但给一句明确的话，省得以为是 bin 本身坏了。*/
    {
        int c = msc_disk_file_contig();

        if (c == 0) {
            kprintf("[dl] ⚠ 文件在 flash 上不连续（首簇 %u）-> 镜像可能被打乱\r\n",
                    (unsigned)first_clu);
        } else if (c < 0) {
            kprintf("[dl] ⚠ 目录项里读不到文件信息\r\n");
        }
    }

    rc = app_image_check(S31_APP_LOAD_BASE, fsize, &hdr, &crc);
    if (rc != 0) {
        kprintf("[dl] image INVALID (rc=%d) -> 拖进来的不是合法 app.bin，留在 bootloader\r\n", rc);
        /* 描述符标记为 BAD，这样下次开机不会去跑一个坏镜像 */
        app_desc_read(&d);
        d.magic = S31_APPDESC_MAGIC;
        d.state = S31_APPDESC_STATE_BAD;
        d.src_off = 0;   /* 镜像固定在 S31_APP_LOAD_BASE，不再需要偏移 */
        d.img_size = fsize;
        app_desc_write(&d);
        return;
    }

    kprintf("[dl] image valid: %u bytes, crc=%08x, entry=0x%08x\r\n",
            (unsigned)fsize, (unsigned)crc, (unsigned)hdr.entry);

    d.magic = S31_APPDESC_MAGIC;
    d.version = 1;
    d.src_off = 0;   /* 镜像固定在 S31_APP_LOAD_BASE，不再需要偏移 */
    /* ★ 存**文件长度**：容器格式下它 == hdr.total_size，扁平格式下它就是整份文件。
     *   两种格式共用这一个字段，开机时再交给 app_image_check() 判格式。 */
    d.img_size = fsize;
    d.img_crc = crc;
    d.entry = hdr.entry;
    d.state = S31_APPDESC_STATE_READY;
    d.name_crc = 0;
    {
        int i;
        for (i = 0; i < 7; i++) {
            d.reserved[i] = 0;
        }
    }
    app_desc_write(&d);
    kprintf("[dl] descriptor written @0x%06x (state=READY)\r\n", (unsigned)S31_APP_DESC_OFFSET);

    kprintf("[dl] rebooting into the App in 1.5 s ...\r\n");
    s31_delay_ms(1500);
    s31_system_reset();
}

/*===========================================================================
 * 主流程
 *===========================================================================*/
int main(void)
{
    uint32_t next_tick = 0;
    uint32_t last_ws;
    uint32_t last_activity = 0;
    uint32_t last_reconnect = 0;
    int busy = 0;
    int have_app;

    s31_usj_hw_init();
    s31_systimer_init();

    kprintf("\r\n================================================\r\n");
    kprintf("  S31 bare-metal MSC bootloader\r\n");
    kprintf("  build %s %s\r\n", __DATE__, __TIME__);
    kprintf("================================================\r\n");

    reset_cause_report();

    /* ★ 先提频到 320MHz，再干别的。
       我们的 EP0/MSC 全在中断里做，40MHz 比 IDF 的 320MHz 慢 8 倍。
       （注：已用 A/B 实验排除"CPU 频率影响 PSRAM 事务"——见 README。）*/
    s31_cpu_clk_boost();
    kprintf("[clk] CPU = %u MHz (mcycle vs SYSTIMER)\r\n", (unsigned)s31_cpu_mhz());

    pin_input_pullup(KEY_GPIO_BOOTLOADER);
    s31_flash_init();

    /* ---- 0a. flash 映射窗口：App 要按 LMA 读自己的 .data 初值 ----
     * 窗口地址 = S31_FLASH_MMAP_BASE + flash 偏移，App 的 LMA（0x40100000）
     * 就落在这里。**必须开机就映好**，否则 App 的 startup.S 一读 __data_lma
     * 就是 store/load access fault。
     * 映射长度给足 8MB 装载区（页大小由 ROM 决定，实际条目数看日志）。*/
    {
        int pages = s31_flash_mmap_window(S31_APP_LOAD_BASE, S31_APP_LOAD_SIZE);
        /* 映完先作废窗口里的旧行：镜像是 SPI1 在 cache 底下写的 */
        s31_flash_mmap_invalidate(S31_APP_LOAD_BASE, 0x10000u);
        kprintf("[mmu] flash 窗口: %08x..+%uKB -> %08x (%d 页)\r\n",
                (unsigned)S31_FLASH_MMAP_BASE, (unsigned)(S31_APP_LOAD_SIZE / 1024),
                (unsigned)S31_APP_LMA_BASE, pages);
    }

    /* ---- 0. PSRAM（App 的 VMA 在 PSRAM，所以 bootloader 必须自己点起来）----
     * 全部初始化在 bsp/s31_psram.c 一个文件里；失败也会**干净返回**，
     * 所以"PSRAM 用不了"绝不会让 bootloader 本身起不来。 */
    g_psram_ok = (s31_psram_init() == 0) ? 1 : 0;
    if (g_psram_ok) {
        uint32_t errs = s31_psram_crc_test(S31_PSRAM_VADDR, 256u);
        kprintf("[psram] pattern test: %s (%u errors)\r\n",
                errs ? "FAILED" : "PASS", (unsigned)errs);
    } else {
        kprintf("[psram] 不可用 -> App 不能放 PSRAM（bootloader 照常跑）\r\n");
    }

    /* ---- 1. 决定进哪个模式 ---- */
    have_app = 0;
    if (key_pressed()) {
        kprintf("[boot] mode key (GPIO%d) held -> BOOTLOADER mode\r\n", KEY_GPIO_BOOTLOADER);
    } else {
        kprintf("[boot] mode key not pressed -> probing App...\r\n");
        have_app = app_probe();
        if (!have_app) {
            kprintf("[boot] no runnable App -> BOOTLOADER mode\r\n");
        }
    }


    /* ---- 2. bootloader 模式：枚举成 MSC ---- */
    kprintf("[boot] starting MSC disk...\r\n");
    msc_disk_init();

    usbd_desc_register(0, &msc_descriptor);
    usbd_add_interface(0, usbd_msc_init_intf(0, &intf0, MSC_OUT_EP, MSC_IN_EP));

    if (usbd_initialize(0, ESP_USB_HS0_BASE, usbd_event_handler) != 0) {
        kprintf("[usb] init FAILED\r\n");
        for (;;) {
            s31_usj_pump();
        }
    }
    s31_usb_interrupt_enable();
    kprintf("[usb] ready: plug the USB-HS cable, drag app.bin onto the S31-BOOT drive\r\n");

    /* ---- 3. 主循环 ---- */
    last_ws = msc_disk_write_sect();
    last_activity = s31_millis();
    last_reconnect = s31_millis();

    for (;;) {
        uint32_t ws;
        uint32_t now;

        s31_usj_pump();

        ws = msc_disk_write_sect();
        now = s31_millis();

        /* 主机把"描述符读取失败"缓存住之后就再也不会重试了
           （设备管理器里是「未知 USB 设备(设备描述符无效)」VID_0000&PID_0005，
            而固件这边一切正常、cfg 永远 0）。
           没人去拔线的话，靠固件自己发一次软拔插把枚举救回来。 */
        if (!(g_usb_events & EVT_CONFIGURED) && (uint32_t)(now - last_reconnect) >= RECONNECT_EVERY_MS) {
            last_reconnect = now;
            kprintf("[usb] 主机一直没配置（cfg=0）-> 软拔插重试\r\n");
            s31_usb_soft_reconnect();
        }

        if (ws != last_ws) {
            if (!busy) {
                busy = 1;
                kprintf("[dl] download started\r\n");
            }
            last_ws = ws;
            last_activity = now;
        } else if (busy && (uint32_t)(now - last_activity) >= DOWNLOAD_IDLE_MS) {
            busy = 0;
            download_finalize();
            last_ws = msc_disk_write_sect();
            last_activity = s31_millis();
        }

        /* 每 5 秒状态 */
        if ((int32_t)(now - next_tick) >= 0) {
            next_tick = now + 5000u;
            kprintf("[t=%6u ms] cfg=%u rd=%u wr=%u flash(erase=%u prog=%u) data_hi=%u evt=%02x irq=%u gint=%08x dsts=%08x\r\n",
                    (unsigned)now,
                    (unsigned)((g_usb_events & EVT_CONFIGURED) ? 1u : 0u),
                    (unsigned)msc_disk_read_sect(), (unsigned)ws,
                    (unsigned)msc_disk_erase_cnt(), (unsigned)msc_disk_prog_cnt(),
                    (unsigned)msc_disk_data_hi(),
                    (unsigned)(g_usb_events & 0xFFu), (unsigned)g_usb_irq_cnt,
                    (unsigned)s31_usb_gintsts(), (unsigned)s31_usb_dsts());
        }
    }
    return 0;
}



