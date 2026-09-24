# ESP32-S31 裸机 MSC 拖拽式 Bootloader

> **把 `.bin` 拖进 U 盘，就烧好了。**
> 板子插上电脑 → 按住 GPIO0 复位 → 电脑上出现一个 U 盘（卷标 `S31-BOOT`）→
> 把 `app.bin` 拖进去 → 1.5 秒后板子自动复位、校验、运行。

**全裸机**：bootloader 和 App 都**不依赖 ESP-IDF、不跑 FreeRTOS、不链 newlib**。
上电后由芯片 **bootROM 直接加载**本工程的镜像（烧在 flash `0x2000`），
`main()` 之前只有我们自己写的 `startup.S`。

| | |
|---|---|
| 芯片 / 板子 | ESP32-S31-WROOM-3（双核 RISC-V @320MHz）/ 乐鑫 **ESP32-S31-Function-CoreBoard-1** |
| flash / PSRAM | 16 MB (QIO @80MHz) / 16 MB (8 线 DDR @200MHz) |
| USB | **USB-HS**（Type-A）= 拖拽盘；**USB-DBG**（Type-C）= 串口日志 + 烧录 |
| 依赖 | 只用 **RISC-V GCC 工具链**（+ 烧录时用 python / esptool）。**没有 ESP-IDF** |
| 许可 | Apache-2.0（见 [LICENSE](LICENSE)） |

---

## 1. 它是什么

```
                    ┌─────────────── 复位 ───────────────┐
                    │  GPIO0（J2-9）按住接 GND ?          │
                    └───────┬───────────────────┬────────┘
                       按住 │                    │ 不按
                            ▼                    ▼
                 ┌────────────────────┐  ┌─────────────────────┐
                 │  BOOTLOADER 模式    │  │ 描述符里有没有 App ? │
                 │  USB-HS 变成 U 盘   │  └───┬─────────────┬───┘
                 │  拖 bin 进去 → flash │   有 │             │ 没有/CRC 不对
                 └────────────────────┘      ▼             ▼
                                        跳进 App      留在 bootloader
```

- **拖拽烧录**：bootloader 里跑的是 CherryUSB（设备模式）+ 一个**虚拟 FAT16 盘**。
  FAT 的元数据只活在 RAM 里，**数据区直接映射到 flash 的 App 装载区** ——
  所以 Windows 以为在写文件，其实字节直接落进了 flash。
- **写完自动收尾**：检测到"静默 1.5 秒"就认为文件传完了 → 校验 → 写描述符（含 CRC）→ 复位。
- **两种镜像格式都吃**：`objcopy -O binary` 的扁平 bin，或带段表的容器 bin（见 §6）。
- **经典内存模型**：App 的 **LMA 在 flash、VMA 在 PSRAM**（见 §5）。
- **App 有效性防线**：开机比对描述符里的 CRC，不匹配就留在 bootloader，绝不跳进垃圾镜像。
- 板载 WS2812 当心跳（RGB 轮换），不接串口也能看出板子活着。

---

## 2. 硬件与接线

| 板上丝印 | 类型 | 作用 | 必须插吗 |
|---|---|---|---|
| **USB-HS** | Type-A | 拖拽盘（S31 的 USB 2.0 OTG-HS **当设备**）| **要**，否则电脑上看不到盘 |
| **USB-DBG** | Type-C | 串口日志 + esptool 烧录（芯片内置 USB-Serial/JTAG）| 建议插，看日志用 |

**模式键 = GPIO0 = J2 第 9 脚**，旁边第 8 脚就是 GND。低有效、内部上拉。
拿杜邦线/镊子把 9 脚短到 8 脚再复位 = 强制进 bootloader（App 跑飞时的救命稻草）。

> ⚠️ USB-HS 这个口**当设备用时，板上还会往 VBUS 反灌 5V**（它本来是给"当主机"设计的）。
> 实测能正常枚举、能跑满速，但插拔时别让两边的 5V 长时间顶牛。

---

## 3. 快速上手

```powershell
# ① 编 + 烧 bootloader（烧到 flash 0x2000，之后 bootROM 直接加载它）
cd boot_msc_s31
make flash                 # 编译 -> esptool 写 0x2000 -> 复位 -> 读 2 秒日志

# ② 编 App（两个镜像一次产出，见 §6）
cd ..\s31_app
make build                 # -> build\app.bin（扁平，默认拖这个）+ build\app_container.bin

# ③ 拖进去（这一条把"回 bootloader + 等盘 + 拷贝"全自动化了）
make flash                 # 擦描述符 -> 等 S31-BOOT 盘出现 -> 编译 -> 拷进去
```

不想用 make 也行，逻辑都在 `tools\*.ps1`：

```powershell
pwsh -File boot_msc_s31\tools\build.ps1        # 编译+烧录+读日志（-BuildOnly / -Rebuild 可选）
pwsh -File s31_app\tools\build.ps1 -Drag       # 编译 + 拷进 S31-BOOT 盘
```

**手动拖拽**（就是普通复制文件）：

1. 按住 GPIO0 复位 → 资源管理器里出现 `S31-BOOT` 盘（本机是 `Z:`，8 MB）；
2. 把 `app.bin` 拖进去；
3. **停手别碰盘**，等 1.5 秒 —— 板子自己复位进 App。

> 端口默认取 `ESP32_S31_PORT` 环境变量（没有就用 `COM43`）：
> `make monitor PORT=COM7` 或先 `$env:ESP32_S31_PORT='COM7'`。

---

## 4. 它跑起来长什么样（真板串口输出）

### 4.1 bootloader 启动（没有 App 时，停在拖拽模式）

```
ESP-ROM:esp32s31-20251218
Build:Dec 18 2025
rst:0x17 (CHIP_USB_UART_RESET),boot:0x7e (SPI_FAST_FLASH_BOOT)
SPI mode:DIO, clock div:1
load:0x2f000000,len:0x807c
load:0x2f008080,len:0x1f7c
entry 0x2f000000                      ← 到此为止全是 ROM 的，下面是我们的

================================================
  S31 bare-metal MSC bootloader
  build Sep 25 2026 07:10:10
================================================
[rst] core0=0x17 (USB-UART(CDC) request)
[clk] 提频 OK: 320 -> 320 MHz（mcycle 实测；硬件回读 75/75 不可信）status=00000002
[clk] CPU = 320 MHz (mcycle vs SYSTIMER)
[mmu] flash 窗口: 40000000..+8192KB -> 40100000 (128 页)
[psram] === 初始化（16MB 8 线 DDR @200MHz）===
[psram] PMA15 [40000000,+524288KB) 补上写权限: c0000015 -> c000001d
[psram] 芯片 = AP 系列 (vendor 0x0d)
[psram] mode reg: MR0=30 MR4=20 MR8=0b（期望 30/20/0b）
[psram] 参考字: 写 5a6b7c8d 读 5a6b7c8d -> OK
[psram] MMU: 256 页 @50000000 -> PSRAM 物理 0（首项 00000c00）
[psram] 自检: 原始写+原始读 OK / 原始写+cache读 OK -> PSRAM 可用
[psram] pattern test: PASS (0 errors)
[boot] mode key not pressed -> probing App...
[app] descriptor invalid (magic=ffffffff state=4294967295) -> no App
[boot] no runnable App -> BOOTLOADER mode
[boot] starting MSC disk...
[disk] FAT16 16483 sectors (8241 KB), cluster 1024 B x 8192, data 16384 sectors @flash 0x100000
[usb:before] clkrst=00000000 cnnt_otg20=e0800000 ... DWC2 GSNPSID=00000000 ...
[usb:after]  clkrst=00000003 cnnt_otg20=08800000 ... DWC2 GSNPSID=4f54430a ...
[I/USB] dwc2 has 16 endpoints and dfifo depth(32-bit words) is 896
[usb] ready: plug the USB-HS cable, drag app.bin onto the S31-BOOT drive
[t=   106 ms] cfg=0 rd=0 wr=0 flash(erase=0 prog=0) data_hi=0 evt=00 irq=0 ...
```

之后每 5 秒一行状态（看门狗式心跳）：

```
[t=  5106 ms] cfg=1 rd=23 wr=18 flash(erase=0 prog=0) data_hi=2560 evt=05 irq=1025 ...
```

| 字段 | 含义 |
|---|---|
| `cfg` | USB 已配置（`1` = 主机枚举完成）|
| `rd` / `wr` | 主机读/写的扇区数（拖动文件时这两个数会涨）|
| `flash(erase prog)` | 真正落到 flash 的擦除块数 / 编程页数 |
| `data_hi` | 数据区被写到的最高偏移（判断"是不是真写进来了"）|
| `evt/irq/gint/dsts` | USB 事件数 / 中断数 / GINTSTS / DSTS（`dsts` bit[2:1] = 速度）|

### 4.2 拖一个 bin 进去（把 `app.bin` 拷到 `Z:\`）

```
[dl] download started
[dl] quiet for 1500 ms -> finalizing
[dl] file: size=4956 first_clu=5 bin=1
[dl] flushing write cache...
[dl] flush done: flash(erase=2 prog=2)
[img] 未识别到 'S31A' 段表 -> 按**扁平镜像**处理: 4956 B -> 0x50000000
[dl] image valid: 4956 bytes, crc=489cc173, entry=0x50000000
[dl] descriptor written @0x900000 (state=READY)
[dl] rebooting into the App in 1.5 s ...
```

（拖进来不是合法镜像的话，这里会变成
`[dl] image INVALID (rc=..) -> 拖进来的不是合法 app.bin，留在 bootloader`，
并且**不会**跳转 —— 描述符也不会被标成 READY。）

### 4.3 复位后自动跳进 App

```
[boot] mode key not pressed -> probing App...
[app] descriptor OK: size=4956 crc=489cc173 entry=50000000
[app] image OK: 4956 bytes, crc=489cc173, 0 segs, entry=0x50000000
[app] 关掉 USB PHY（避免给主机留僵尸设备），然后跳转
```

```
================================================
  S31 bare-metal App is ALIVE
  build Sep 25 2026 07:41:03
------------------------------------------------
[app] .image VMA 50000000 <- LMA 40100000   [bootloader 搬]
[app] .data  VMA 50800000 <- LMA 40101258   [startup 从 flash 搬]
[app] 布局自检: OK —— LMA 在 flash、VMA 在 PSRAM，初值搬运与 .bss 清零都对
[app] LED: RMT -> WS2812 (GPIO60)，红/绿/蓝 1 秒轮换
================================================
```

App 打完这一屏就**不再输出**了 —— 之后板载 LED 就是心跳（红/绿/蓝每 1 秒轮换）。

> 📌 **复位瞬间可能丢几行日志**：控制台走的是 USB-Serial/JTAG，复位会让它重新枚举，
> 主机重新打开端口之前那几百毫秒的输出会丢。丢的通常是 `[boot] probing App...`
> 那几行，**不是故障**。

---

## 5. 内存布局

### 5.1 flash

```
0x000000 ┌──────────────────────────────┐
         │ bootROM 保留 / 分区表区（不用）│
0x002000 ├──────────────────────────────┤
         │ ★ 本 bootloader（41 KB）      │ ← ROM 从这里加载二级镜像
0x100000 ├──────────────────────────────┤
         │ ★ App 镜像装载区 = 磁盘数据区 │ ← 拖进来的 bin **固定**落这里
         │   （8 MB，映射到 0x40100000） │
0x900000 ├──────────────────────────────┤
         │ App 描述符（4 KB）            │ ← 写完镜像后填：magic/state/size/crc/entry
0x901000 ├──────────────────────────────┤
         │ 空闲                          │
0x1000000└──────────────────────────────┘
```

### 5.2 App 的内存模型：**LMA 全在 flash，VMA 全在 PSRAM**

```
LMA（装载地址 = 初值在哪儿）        VMA（运行地址 = 跑在哪儿）
flash 0x100000，别名 0x40100000     PSRAM
  ├ .boot/.text/.rodata 的初值 ────→ 0x50000000   ← bootloader 整块搬过去，才能取指
  └ .data/.sdata 的初值     ──────→ 0x50800000   ← App 自己的 startup.S 从 flash 搬
                                     .bss/.sbss   ← App 自己清零
                                     栈顶 0x51000000
```

分工一句话：**bootloader 只负责"让入口能跑起来"（整块 bin 拷到 `0x50000000`）；
变量初值仍然留在 flash 里，由 App 的 `startup.S` 自己搬。**

这套模型要成立，有两个**前提**，缺一个都跑不起来：

| 前提 | 谁做 | 为什么 |
|---|---|---|
| ① 镜像必须落在**固定** flash 地址 | `boot_msc_s31/app/msc_disk.c` 的 `data_map_off()` | App 的 `la t0, __data_lma` 是**链接期常量**，而 FAT 把文件分到哪个簇是**运行期**才知道的 |
| ② flash 必须**映射**给 App 读 | `boot_msc_s31/bsp/s31_flash.c` 的 `s31_flash_mmap_window()` | `__data_lma = 0x401013a0` 这种地址本质是 flash 的别名，不映射 = 一读就 `load access fault` |

**① 的实测依据**：Windows 一挂载卷就会建 `System Volume Information`，占掉簇 2~4，
所以拖进去的 `app.bin` **首簇是 5**（不是 2）。所以映射按"相对首簇"折算：

```
镜像内偏移 =（簇号 − 首簇号）× 簇大小 + 簇内偏移
    首簇 = 5  →  簇 5,6,7… 依次映射到 0x100000, 0x100400, 0x100800…
```

这样无论 FAT 把文件放在哪，镜像都**固定**落在 `0x100000`。

---

## 6. 两种镜像格式

| | 扁平 bin（**推荐**）| 容器 bin |
|---|---|---|
| 怎么产 | `objcopy -O binary app.elf app.bin` | `tools/mkapp.py` 打包 |
| 长什么样 | 纯数据流，`[.image 初值][.data 初值][.sdata 初值]` | 64 B 头 + 8×16 B 段表 + payload（magic `'S31A'`）|
| bootloader 怎么处理 | 当作 `0x40100000...` 的连续镜像，**整块**拷到 `0x50000000` | **按段表逐个搬/清零**（COPY 到 VMA、ZERO 清零）|
| 链接基准 | `.image` LMA = `0x40100000` | `.image` LMA = `0x401000C0`（多出 192 B 头表）|
| 适用 | 绝大多数情况 | App 的运行位置不止一处、扁平表达不了的时候 |

`make build` 在 `s31_app` 里**一次产两个**：`build\app.bin` 和 `build\app_container.bin`，
想拖容器那个就 `make drag BIN=app_container.bin`。

为什么容器格式的链接基准要往后挪 192 字节：App 的 `startup.S` 是按
「**文件内偏移 == flash 装载偏移**」去读 `.data` 初值的。容器前面有头+段表，
payload 整体后移，基准不跟着挪就会读到段表 —— 实测 `magic` 读成 `0x00000000`。

---

## 7. 两个工程

### 7.1 `boot_msc_s31` —— 裸机 bootloader

| make 目标 | 作用 |
|---|---|
| `make build` / `rebuild` | 编译（增量 / 全量）→ `build\app.bin`（41 KB）|
| `make flash` | 编译 + esptool 写 `0x2000` + 复位 + 读 2 秒日志 |
| `make monitor` | 看串口（`SECONDS=8` 只看 8 秒，缺省一直看）|
| `make run` | `flash` 然后 `monitor` |
| `make erase` | 擦掉 App 描述符（`0x900000`）→ 板子回到 bootloader 模式 |
| `make erase-all` | 整片擦除（连 bootloader 一起，之后要重新 `make flash`）|
| `make size` / `clean` / `headers` | 段大小 / 清 build / 重新冻结 IDF 头 |

组成：CherryUSB（USB-HS 设备模式 + MSC 类）、虚拟 FAT16 盘、flash 读/擦/写、
PSRAM 初始化、MMU 映射、CLIC 中断、USB-Serial/JTAG 控制台 —— **全部裸机**。
代码量与文档索引见 [`boot_msc_s31/README.md`](boot_msc_s31/README.md)。

### 7.2 `s31_app` —— 裸机 App 模板

| make 目标 | 作用 |
|---|---|
| `make build` / `rebuild` | 编译 → `build\app.bin` + `build\app_container.bin` |
| `make flash` | 擦描述符 → 等 `S31-BOOT` 盘 → 编译 → 拷进去（全自动"烧录"）|
| `make drag` | 只拷贝（板子要已在 bootloader 模式）|
| `make monitor` / `run` / `erase` / `size` / `clean` / `headers` | 同上 |

**开机自检**：App 起来会打一屏，最后一行 `布局自检: OK` 是四条判据全过的结论 ——
装载地址在 flash 段、变量落在 PSRAM 段、`.sdata` 搬运对、`.data` 搬运对且 `.bss` 为 0。
看到 OK 就说明 **flash 映射窗口 + LMA 搬运链路**整条是通的。

---

## 8. 目录结构

```
esp32s31_msc_bootloader/
├── README.md                  ← 你正在看的
├── LICENSE                    Apache-2.0
├── boot_msc_s31/              裸机 bootloader
│   ├── README.md              设计 / 布局 / 上手 / 踩坑（先读这个）
│   ├── PSRAM.md               ★ PSRAM 专章：742 行，含"已排除、别重查"的方向
│   ├── DEBUG-LOG.md           按轮次的排查流水账（历史与结论分开）
│   ├── app/  bsp/             bootloader 源码（msc_disk / app_image / main ...）
│   ├── cherryusb/             CherryUSB（随工程走，不依赖组件管理器）
│   ├── bsp/idf_headers/       冻结的 45 个 IDF 头（PSRAM 那条 include 链）
│   └── tools/                 build.ps1 / make.ps1 / read_port.py / sync_idf_headers.ps1
└── s31_app/                   裸机 App 模板
    ├── README.md              怎么编、怎么拖、自检怎么读、怎么排错
    ├── app/main.c             样板：段布局自检 + LED 心跳
    ├── bsp/                   linker.ld / startup.S / RMT 驱动 / 控制台 / 陷阱处理
    │   └── idf_headers/       冻结的 12 个 IDF 头（RMT 那条 include 链）
    └── tools/                 build.ps1 / mkapp.py / make.ps1 / read_port.py
```

**自包含**：两个工程都把需要的 IDF 头**冻结**在自己目录里（用 `gcc -M` 问编译器要
include 闭包，一次性拷进来），所以**不需要 ESP-IDF 源码树**，只要有 RISC-V 工具链就能编。
理由：S31 是 preview target，寄存器位域跟老芯片不一样，**按 IDF 的头文件里的字段名写，
不手抄位号** —— 作者为此栽过两次（`MSPI_DIV.FB_DIV` 的位号、PSRAM 的 MR0/MR4/MR8）。

---

## 9. 踩过的坑（挑最硬的几条）

> 完整清单在各自 README / PSRAM.md / DEBUG-LOG.md 里，这里只挑"不看会重踩"的。

1. **PSRAM 写不进去 ≠ PSRAM 没初始化好。** 真凶是 **PMA**（RISC-V 自定义 CSR，
   `CSR_PMACFG15`）把外部存储窗口标成了只读 —— 它是 CPU 的 CSR，**寄存器 dump 里根本看不到**。
   修法是给那一段补上写权限（日志里那行 `PMA15 ... c0000015 -> c000001d`）。
2. **别手抄寄存器位号。** 上面那条和 PSRAM 的 mode register 都是这么栽的。用 IDF 头里的字段名。
3. **虚拟盘的"文件放哪"是运行期才知道的**，而 App 的 `__data_lma` 是链接期常量
   → 必须按"相对首簇"折算（见 §5.2 ①），否则镜像会落在 `0x100C00` 这种地方，App 直接跑飞。
4. **扁平镜像没有任何元数据**，光看"长度对不对"拦不住陈旧描述符
   → 开机必须比对描述符里的 CRC，否则会把垃圾装进 PSRAM 然后跳进去
   （现象是开机横幅之后串口全无，作者靠 OpenOCD 读 `PC=0`、反查 `ra` 才定位）。
5. **USB 设备的 PHY 跨复位保留** → 跳 App 前必须显式关掉，否则主机会一直记着一个僵尸设备。
6. **别在 USB 中断里打日志。** 同步写 UART 会阻塞几百微秒～几毫秒，
   等时端点那一包就永久丢了（听感是"咔咔"声，而计数全绿看不出来）。
7. **灯不亮/常亮白 ≠ 时序参数抄错了。** WS2812 用 RMT 驱动时，符号必须写进
   **`RMTMEM`（`0x20355800`）** 并且 `apb_fifo_mask=1`；写进 `RMT.chndata`（FIFO 路径）
   TX 引擎根本收不到数据，现象是"一帧只花 5µs 就发完"。
8. **自己加的 make 目标要逐个真跑一遍。** `make erase-all` 写错了整整一天没人发现，
   因为"怕把板子擦成砖"一直没跑 —— 结果它从一开始就是坏的
   （S31 的 ROM 在 `--no-stub` 下不支持 `erase_flash`，只能 `erase-region`）。

---

## 10. 工具链 & 依赖

| 用途 | 需要什么 |
|---|---|
| 编译（两个工程） | **riscv32-esp-elf-gcc**（本仓库用 `esp-15.2.0_20251204`，IDF 自带的那份即可）|
| 烧 bootloader | python + `esptool`（要能识别 esp32s31 的版本）+ 板子插 USB-DBG |
| 拖 App | **什么都不用** —— 就是往 U 盘里拷文件 |
| PowerShell 脚本 | `make` 只是薄封装，逻辑在 `tools\*.ps1`（Windows PowerShell 5.1+ / pwsh 7+）|

编译 App 的命令（不想用 make 时）：

```powershell
pwsh -File s31_app\tools\build.ps1
```

烧 bootloader 的等价命令：

```powershell
python -m esptool --chip esp32s31 -p COM43 -b 460800 --no-stub `
       write-flash -fm dio -ff 80m -fs 16MB 0x2000 boot_msc_s31\build\app.bin
```

---

## 11. 恢复成 ESP-IDF 启动

本工程占用了 flash `0x2000`（二级镜像的位置）。想跑回 IDF 的工程：

```powershell
# 在 IDF 工程里
idf.py --preview -p COM43 flash
```

或者 `make erase-all` 整片擦掉再重烧 IDF 的 bootloader + partition-table + app。

---

## 12. 致谢 / 参考

- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) —— USB 协议栈（Apache-2.0）
- [ESP-IDF](https://github.com/espressif/esp-idf) —— 冻结进来的寄存器头文件、
  以及作为"已知良好基线"的对照工程（Apache-2.0）
- 参考过的拖拽 IAP 思路：`stm32f103_cherryusb_drag_n_drop_iap`（STM32 版 FAT16 虚拟盘）

---

**作者**：minichao9901 ｜ 问题/建议请开 issue。
两个工程的细节文档在 [`boot_msc_s31/README.md`](boot_msc_s31/README.md) 和
[`s31_app/README.md`](s31_app/README.md)。
