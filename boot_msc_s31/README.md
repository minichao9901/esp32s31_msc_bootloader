# boot_msc_s31 —— ESP32-S31 裸机 CherryUSB MSC 拖拽烧录 bootloader

> 目标：**不依赖 ESP-IDF / FreeRTOS** 的 bootloader，上电时用按键决定进 bootloader 还是进 App；
> bootloader 模式下 USB-HS 口枚举成一个 FAT16 小盘，把 `app.bin` 拖进去就写进 flash。
> 支持的 App 是 **LMA 在 flash、VMA 在 PSRAM** 的裸机镜像。
>
> 📌 **PSRAM 那一大块（耗时最久、弯路最多）单独写在 [PSRAM.md](PSRAM.md)** ——
> 改 PSRAM 相关代码前先读它，里面列了"已经排除掉、不要再查一遍"的方向。

---

## 1. 当前进度（全部真板实测，2026-09-25）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| 裸机启动（bootROM 直启，无 IDF） | ✅ | ROM 日志 `load:0x2f000000 / entry 0x2f000000` |
| 控制台（USB-Serial/JTAG，COM43） | ✅ | 开机横幅 + 周期状态行 |
| **CPU 40MHz → 320MHz（CPLL）** | ✅ | `[clk] CPU = 320 MHz`（mcycle×SYSTIMER 实测，切频前后正好 8 倍）|
| SYSTIMER 时基（16MHz，与 CPU 频率无关） | ✅ | `delay_ms(10) -> 10 ms` |
| flash 读 / 擦 / 写 | ✅ | 擦后读回 `ffffffff`、写后读回逐字节 MATCH |
| **USB-HS 枚举成 MSC** | ✅ | Windows：`USB 大容量存储设备`，盘符 `Z:`，卷标 **S31-BOOT** |
| **拖拽写入落 flash** | ✅ | `wr=29 flash(erase=2 prog=2) data_hi=7168` |
| 写完自动判定 + 校验 + 描述符 | ✅ | 非法文件被正确拒绝（`bad magic 0x03020100`） |
| **PSRAM 初始化** | ✅ M3 | **单文件 `bsp/s31_psram.c`**。真凶有两个：mode register 三个位域写错 + **PMA 把外部存储窗口标成只读**（详见 **[PSRAM.md](PSRAM.md)**）|
| **PSRAM 写** | ✅ M3 | 真凶是 **PMA**（CPU 自定义 CSR，`PMACFG15`），修好后 cache 写 PSRAM 正常；装 App 仍走原始 MSPI 事务 |
| **App 搬运 + 跳转** | ✅ M4 | 拖入镜像 → 校验 CRC → 搬到 VMA → 跳转 |
| **扁平 objcopy 镜像直接拖** | ✅ M6 | bootloader 支持两种格式：`'S31A'` 容器 / **纯 `objcopy -O binary` 产物**（§3）。`s31_app` 现在**默认就产这个** |
| **★ 经典模型：LMA 全在 flash、VMA 全在 PSRAM** | ✅ **M7（2026-09-25）** | 镜像**固定**落 `0x100000`（`data_map_off` 按"相对文件首簇"折算，实测 `first_clu=5` 也照样落对）；开机把该段映射到 `0x40100000`；bootloader 只整块搬到 `0x50000000`，`.data/.sdata` 由 App 的 `startup.S` **从 flash** 搬到 `0x50800000`。真板日志见 §3.0 |
| App 开机 CRC 复验 | ✅ M7 | `app_probe()` 里比对描述符里的 `img_crc`；不匹配就留在 bootloader（**扁平格式唯一的防线**，见 §3.1 末尾的坑） |

镜像大小约 41KB / bss 155KB（大头是 MSC 的 64KB bounce buffer + FAT 33KB），
链接脚本 ASSERT 保证 bootloader 停在 `0x2F060000` 以下，把上面的 RAM 留给 App。

---

## 2. 快速上手

```powershell
cd boot_msc_s31
make help          # 目标一览
make build         # 编译（增量）
make flash         # 编译 + 烧录到 flash 0x2000 + 读 2 秒日志
make monitor       # 一直看串口（Ctrl+C 退出；只看一段：make monitor SECONDS=8）
make run           # flash 然后 monitor
make erase         # 擦掉 App 描述符 -> 板子回到 bootloader 模式
make erase-all     # 整片擦除（连 bootloader 一起，之后要重新 make flash）
```

> `make` 只是薄封装，真正干活的是 `tools\build.ps1` / `tools\make.ps1`，
> 不想用 make 就直接调它们：
> `pwsh -File tools\build.ps1`（编译+烧录+读日志）、`-BuildOnly`（只编译）、`-Rebuild`（全量重编）。
> 端口取环境变量 `ESP32_S31_PORT`（没设就用 `COM43`；也可以临时 `make monitor PORT=COM7`）。

硬件：**两根线**
- `USB-DBG`（Type-C）= 烧录 + 日志（本机 COM43）
- **`USB-HS`（Type-A）= 当设备用的那个口，必须插到 PC**，否则电脑上不会出现盘

模式键：**GPIO0 = J2 第 9 脚**（旁边第 8 脚就是 GND），低有效、内部上拉。
按住它上电/复位 → 强制进 bootloader；不按 → 有合法 App 就跳 App，否则还是进 bootloader。

写镜像：把 `app.bin` 拖到 `S31-BOOT` 盘 → 静默 1.5 秒后自动校验 → 通过就写描述符并复位进 App；
不通过就在日志里说明原因并留在 bootloader（同时把描述符标成 BAD，避免下次去跑坏镜像）。
（App 侧的工程在同级目录 `s31_app\`，它有自己的 Makefile，`make flash` 能把这套拖拽全自动化。）

**恢复 IDF 启动**：本工程占用了 flash `0x2000`（二级镜像位置）。
在任意 ESP-IDF 工程里 `idf.py --preview -p COM43 flash` 即可把它覆盖回去，
或者 `make erase-all` 整片擦掉再重烧。

---

## 3. Flash 布局与 App 装载模型（`bsp/s31_layout.h`）

```
0x000000 ┌────────────────────────────┐
         │ ROM 保留 / 分区表区（不用） │
0x002000 ├────────────────────────────┤
         │ 本 bootloader 镜像          │ ← ROM 从这里加载二级镜像（留到 0x0FFFFF）
0x100000 ├────────────────────────────┤
         │ ★ App 镜像装载区 = 磁盘数据区│ ← 拖进来的 bin **固定**落这里（8MB）
         │   映射到 0x40100000 当 LMA   │
0x900000 ├────────────────────────────┤
         │ App 描述符块（4KB）         │ ← 写完镜像后填；开机据此判断"有没有程序"
0x901000 ├────────────────────────────┤
         │ 空闲                        │
0xF00000 ├────────────────────────────┤
         │ flash 自检/临时区           │
0x1000000└────────────────────────────┘
```

### 3.0 ★ App 的内存模型：**LMA 全在 flash，VMA 全在 PSRAM**（经典 bootloader 模型）

```
LMA（装载地址 = 初值在哪）        VMA（运行地址 = 跑在哪）
flash 0x100000，别名 0x40100000   PSRAM
  ├ .boot/.text/.rodata 的初值 ──→ 0x50000000  ← **bootloader 整块搬过去**才能取指
  └ .data/.sdata 的初值    ──────→ 0x50800000  ← **App 自己的 startup.S 搬**
                                     .bss/.sbss  ← App 自己清零
```

分工就一句话：**bootloader 只负责"让入口能跑起来"（把整块 bin 拷到 `0x50000000`），
变量初值还留在 flash 里，由 App 的 `startup.S` 自己搬。**

这套模型要成立，有两个**前提**，缺一个都跑不起来：

| 前提 | 谁做 | 为什么 |
|---|---|---|
| ① 镜像必须落在**固定** flash 地址 | `msc_disk.c` 的 `data_map_off()` | App 的 `la t0, __data_lma` 是**链接期常量**，而 FAT 把文件分到哪个簇是**运行期**才知道的 |
| ② flash 必须**映射**给 App 读 | `s31_flash.c` 的 `s31_flash_mmap_window()` | `__data_lma` = `0x401013a0` 这种地址，本质是 flash 的别名；不映射 = 一读就 `load access fault` |

**①的实测证据**：Windows 挂载卷时会自己建 `System Volume Information`，占掉簇 2~4，
所以拖进去的 `app.bin` 首簇是 **5**（不是 2）。老实现按"数据区绝对偏移"写 flash，
镜像会落在 `0x100000 + 3×1024 = 0x100C00`，LMA 对不上、App 直接跑飞。
现在按**相对文件首簇**折算：

```
镜像内偏移 = (簇号 - 首簇号) × 簇大小 + 簇内偏移
   首簇 = 5  →  簇 5,6,7… 也映到 0x100000,0x100400,0x100800…
```

⇒ **不管 FAT 分了哪个簇，镜像永远落在 `0x100000`**（真板日志：`first_clu=5`，
`[app] .image VMA 50000000 LMA 40100000` ✅）。

⚠️ 这个折算**要求文件在 flash 上连续**。卷是每次开机用 RAM 里的全空 FAT 重建的，
Windows 从最低空闲簇顺序分配，单文件必然连续；`msc_disk_file_contig()` 还会再验一遍
（不连续 → 校验必失败 → 留在 bootloader，不会跑飞）。

**②的实测日志**：`[mmu] flash 窗口: 40000000..+8192KB -> 40100000 (128 页)`
（flash MMU 页大小 64KB，由 ROM 决定，代码用 `mmu_ll_get_page_size()` 读回来算，没写死）。
翻录用的是 ROM 的 SPI1（**从 cache 底下穿过去**），所以写完必须
`s31_flash_mmap_invalidate()` 作废映射窗口里的旧 cache 行。

**App 侧对应的自检打印**（`s31_app` 每次开机都打，可以直接当验收）：

```
[app] .image  VMA 50000000  LMA 40100000  [bootloader 整块搬]
[app] .text   VMA 5000017c..50000d3a  (3006 B)
[app] .rodata VMA 50000d40..5000139e  (1630 B)
[app] .data   VMA 50800000..50800100 (256 B)  LMA 401013a0  [startup 从 flash 搬]
[app] .sdata  VMA 50800100..50800104 (4 B)  LMA 401014a0  [startup 从 flash 搬]
[app] .bss    VMA 50800108..50801318  (4624 B)   [startup 清零]
[app] 初值真的从 flash 搬过来了吗？ magic=1234abcd blob[0]=0badf00d -> 是
```

> 📌 **`first_clu` 现在只是日志**：`download_finalize()` 里仍然会打它，
> 但镜像落在哪儿跟它无关了。描述符里的 `src_off` 也固定写 0。

### 3.1 App 镜像格式 —— 支持两种，bootloader 按开头 4 字节自动识别

| | **格式 A：带段表的容器** | **格式 B：扁平的 objcopy 产物**（★ 推荐） |
|---|---|---|
| 怎么来 | `tools/mkapp.py app.elf app.bin` | `riscv32-esp-elf-objcopy -O binary app.elf app.bin` |
| 识别依据 | 开头 4 字节 == `'S31A'` | **其它一律当格式 B** |
| 结构 | 64B 头 + 8×16B 段表 + 各段 payload | 纯数据，**没有任何元数据** |
| 装载地址 | 每段各自 `vma` | 整份文件装到 `S31_APP_FLAT_VMA`（= `0x50000000`） |
| 入口 | 头里的 `entry` | `S31_APP_FLAT_VMA`（镜像第 0 字节） |
| 长度 | 头里的 `total_size` | FAT 目录里的文件长度 |
| 完整性 | 头里带 `crc32`，拖拽时当场比对 | **头里没地方存 CRC** → 拖拽时只算出来存进描述符，开机时再比对 |
| `.bss` 清零 | 有专门的 ZERO 段，bootloader 负责 | **由 App 自己的 `startup.S` 清零**（objcopy 不输出 NOBITS） |
| 能表达 | 多区域 VMA / 精确入口 / bss 段 | 单一连续区域 |

> `s31_app` 现在**两个格式都产**（`make build` 一次出两个，都能拖）：
> `build\app.bin`（格式 B，默认拖这个）和 `build\app_container.bin`（格式 A）。
> 注意两者的**链接基准不一样** —— 容器多 192 字节头，App 的 `startup.S` 是按
> 「文件内偏移 == flash 装载偏移」读 `.data` 初值的，所以那次链接用
> `--defsym=S31_LMA_BASE=0x401000C0` 把基准也挪了同样多。
> 详见 [../s31_app/README.md](../s31_app/README.md)。

格式 A 的 `vma` 只能落在 PSRAM `0x50000000~0x54000000` 或 App 内部 RAM 区
`[0x2F060000, 0x2F07AFC0)`（校验时会检查，越界直接拒绝）。

**格式 B 的约定**（四条，缺一不可）：
1. 链接脚本把 `.boot`（含 `_start`）放在**最前面** —— 这样 `objcopy` 的输出
   第一字节就是入口；
2. 镜像第 0 字节的 VMA == `S31_APP_FLAT_VMA`（= `0x50000000`），
   而它的 LMA == `S31_APP_LMA_BASE`（= `0x40100000`）；
3. `.bss` 由 App 自己清（`startup.S` 里本来就有这段）；
4. 🚨 **`objcopy` 是按 LMA 排布字节的，而整块 bin 会被原样拷到 `0x50000000`**，
   所以**镜像前半段**（bootloader 要搬的那些）必须满足
   「文件内偏移 == 相对 `0x50000000` 的偏移」。
   做法：把它们放进**一个**输出段：

   ```ld
   .image : { .boot / .clic_entry / .text / .rodata } > IMG AT> FLASH
   .data  : { ... } > DATA AT> FLASH      /* LMA 紧跟在 .image 后面，也在 flash */
   ```

   单个输出段里 VMA 和 LMA 是同一块内容的两个基址，偏移一一对应，扁平镜像天然就对。
   （`.data`/`.sdata` 的初值虽然也在同一个 LMA 流里，但它们会被拷到 `0x50001xxx`
   这种**没人用的地方** —— 无害，真正生效的是 `startup.S` 从 flash 那次搬运。）

   ❌ **不能**按段分别写 `> IMG AT> FLASH`：ld 会按"上一段 LMA + 段大小"紧凑分配
   LMA，**不镜像 VMA 侧的对齐空隙**。实测踩过：`.clic_entry` 的 LMA 落在
   `0x00100082`、VMA 却在 `0x500000c0`（差 62 字节）→ 扁平镜像变成 4188 B
   而 VMA 跨度是 4252 B → 整块拷到 `0x50000000` 时 `.text` 整体错位 → **必崩**。
   `s31_app/tools/build.ps1` 里有一条守门检查（`app.bin` 大小必须 ==
   各段 LMA 的总跨度），改坏了会直接编译失败。

> 板上实测（2026-09-25，S31 真板，`first_clu=5`）：
> `app.bin` 5284 B → 拖进去 → 自动复位 → App 起来，
> `.image LMA=40100000` / `.data LMA=401013a0` / 初值搬运全部核对通过 ✅

⚠️ 因为"非 `'S31A'` 一律当扁平镜像"，拖一个完全无关的 `.bin` 也会被装到
`0x50000000` 并跳过去 → 会跑飞。恢复很简单：**按住 GPIO0（J2-9 接 GND）上电**
就进 bootloader 模式，重新拖一个正确的即可。

> 🚨 **跳进 App 之前一定要先清描述符里的旧状态**：`app_probe()` 是信描述符的。
> 如果描述符说 READY、而 flash `0x100000` 里其实是别的东西（比如上一次拖失败的残留、
> 或者一串 FAT 目录项），扁平格式**没有可校验的魔术字/CRC**，会被当成合法镜像装进
> PSRAM 并跳过去 —— 现象是**串口在开机横幅之后就没声了**（本人在排查时被它带偏了
> 一整轮，最后靠 JTAG 抓住 `PC=0x00000000`、`ra` 落在 `app_jump` 才定位到）。
> 真要复位到干净状态：把描述符区 `0x900000` 写回 `0xFF` 即可。

---

## 4. 已核对的 S31 硬件事实（都有出处）

| 项 | 值 | 出处 |
|---|---|---|
| RAM 窗口 | `0x2F000000 .. 0x2F07AFC0` | IDF `soc.h` / `ld.hp_mem_defs` |
| 二级镜像偏移 | flash **0x2000** | esptool `BOOTLOADER_FLASH_OFFSET` |
| App 内部 RAM 起点（本工程约定） | `0x2F060000` | 本工程 linker.ld |
| PSRAM 虚拟窗口 | `0x50000000`（64MB 窗口） | IDF `soc.h` `SOC_EXTRAM_LOW/HIGH` |
| USB OTG HS（DWC2） | **0x20300000**，中断源 **99** | `reg_base.h` / `interrupts.h` |
| DWC2 IP | `GSNPSID=4f54430a`、`GHWCFG2=239ffed2`、`GHWCFG4=de11aa30`、16 端点/896 words | 真板实测，**与 P4 完全一致** |
| USB 引脚/PHY 控制 | `HP_SYS_CLKRST+0xAC`、`CNNT_SYS+0x30`、`HP_ALIVE_SYS+0xB0` | IDF `usb_utmi_ll.h` |
| USB-Serial/JTAG | **0x20391000**（中断源 2） | `reg_base.h` |
| SYSTIMER | **0x20399000**，16MHz（XTAL 40MHz÷2.5），中断源 33 | `reg_base.h` / IDF 注释 |
| TIMG0（复位用） | **0x20580000** | `reg_base.h` |
| CLIC | `0x10800000`/`0x10801000`，外部中断从 ID 16 起，阈值用 `mintthresh` CSR(0x347) | `clic_reg.h` / rtt 实测 |
| 中断路由矩阵 | `0x20585000 + 4*source` ← CLIC ID | `interrupt_clic_ll.h` |
| 复位原因 | `LP_CLKRST+0x30`，bit0=flag、bit[6:1]=cause，写 bit30 清 | rtt 实测 |

---

## 5. 踩过的坑（每条都是真板换来的，改代码前先读）

### 5.1 🚨 ROM 认为板载 flash 只有 2MB —— 擦/写全部失败
反汇编 ROM 的 `esp_rom_spiflash_erase_sector`（真实实现 @`0x2f80ef70`）可以看到它先做
`sector_num < chip_size/sector_size` 的**范围检查**，不满足就直接返回 `ERR` 且**根本不碰 flash**。
而 bootROM 交棒时 `g_rom_flashchip.chip_size = 0x200000`（2MB，512 个扇区），
所以擦 `0xFFF000`（第 4095 扇区）必然失败。

现象极具误导性：**"擦失败，而且之后连读都失败"**（读也有同样的范围检查），
很容易误判成"cache 挂起把 SPI1 搞坏了"。

修法：开机调 IDF 自己用的 `esp_rom_spiflash_config_param(device_id, 16MB, 64KB, 4KB, 256, 0xFFFF)`。

顺便实测结论：**flash 写/擦不需要挂起 cache**（ROM 的 `esp_rom_spiflash_*` 是包装版，
自己会处理 SPI1 与 cache 的关系；本工程代码/数据又都在内部 RAM）。
所以 `s31_flash.c` 里只关中断，没有 `Cache_Suspend_*` 那一套。

### 5.2 🚨 CNNT_SYS 的 USB 复位默认是"按住"的，而且复位期间写寄存器会被吃掉
`CNNT_SYS.sys_usb_otg20_ctrl`（`0x20359030`）复位默认值 = `0xE0800000`：
`phy_rst/ahb_rst/apb_rst` 三位**全是 1**。而**复位按住期间对该寄存器的写会被吃掉、读回 0**。

所以"先开时钟再放复位"（照 IDF 的调用顺序抄）会把时钟位一起写没 →
控制器永远没时钟 → `GSNPSID/GHWCFG` 全 0 → CherryUSB 打印
`device_rx_fifo_size cannot be larger than power_on_value 0` 然后死等。

**正确顺序：先放复位（先 PHY 后控制器），再开 UTMI/PHYREF 时钟。**

### 5.3 🚨 soc 结构体里的 `uint32_t reserved_xxx[N];` 没有 volatile，漏算会让偏移整体错 8 字节
`CNNT_SYS` / `HP_ALIVE_SYS` / `LP_SYS` 的寄存器偏移必须逐字段累加算出来，
而结构体里混着**不带 `volatile`** 的 `reserved` 数组。漏掉它们 → 偏移少 8 字节 →
把 USB 时钟写到隔壁寄存器上（症状同上：控制器没时钟）。
本工程的做法：从 IDF 的 `*_struct.h` 逐字段累加算出偏移（当时用过一个
`tmp/soc_extract.py` 脚本，**已经删掉了**；现在这几个偏移就在 `s31_regs.h` 里，
每个都标了出处），再用 OpenOCD 读真板逐个核对 ——
`0x2035902c=0x27130900`、`0x20359030=0xe0800000`、`0x20359034=0x40000000`
就是"偏移算对了"的指纹。

### 5.4 🚨 变参 width 必须和调用方实际传的类型一致
`kprintf` 里 `%x/%u/%d` 一律按 **32 位**（`long`/`unsigned long`）取参，只有写了 `ll` 才按 64 位取。
早期版本统一按 `unsigned long long` 取 → 参数读取整体错位 → `%s` 拿到野指针 `0x2F` →
`Load access fault (mcause=0x30000005, mtval=0x0000002f)`。
（RISC-V ilp32：32 位实参只占 1 个寄存器，按 8 字节取会连下一个参数一起吃掉。）

### 5.5 🚨 死循环里不刷控制台 = 日志永远看不到
发送缓冲是 4KB 环形 + 64 字节 FIFO，只在 `kprintf`/`s31_usj_pump` 时灌进 FIFO。
一旦固件卡在别人的死循环里（比如 CherryUSB 那个等控制器就绪的 `while(1)`），
环形缓冲里剩的几百字节就永远发不出去 —— 只能看到半截日志，非常误导。

两件事都做了：
- `s31_delay_ms()` 里每毫秒调一次 `s31_usj_pump()` → **任何带延时的忙等都会持续输出日志**；
- 异常停机循环里也一直 pump。

（试过用 "TX FIFO 空" 中断做后台排空：那个中断是**电平型**的，FIFO 一直空就一直置位，
清 `INT_CLR` 也清不掉 → CLIC 那一路一直 pending → 反复重入 → 主循环被饿死。
此路不通，已回退。）

### 5.6 其它
- **看门狗**：`startup.S` 里除了关 RTC_WDT/TIMG0-1，还必须打开**超级看门狗自动喂狗**
  （`RTC_WDT_SWD_CONFIG` bit18），否则因为 RTC_WDT 被关掉，SWD 每 ~3.4 秒整片复位一次。
- **USJ 的 `wr_done` 每次都写**，哪怕一个字都没写：装满 64 字节的 FIFO 会被硬件自动提交，
  主机把它当"未结束的 USB 事务"，要补一个零长度包才会交给 CDC 读端。
- **打开串口会复位芯片**（USB-JTAG 的 DTR/RTS 就是复位线），所以读日志前先把
  当前输出抓完，别指望"读一次拿全部"。
- **每次复位后盘符可能变**（Windows 重新分配）；卷标固定是 `S31-BOOT`，按标签找盘。
- **中文日志在 GBK 终端里是乱码**，别去改固件 —— 是终端编码的事，
  `tools/read_port.py` 已经强制 UTF-8 + `errors=replace`。
- **别往日志里放 GBK 编不了的符号**（`✔ ✘ ✓`），会让读取脚本直接崩。

### 5.7 USB 枚举稳定性（三件事一起做才稳）

这三条都是**真板反复插拔换来的**，详细现场见 [DEBUG-LOG.md](DEBUG-LOG.md)：

1. 🚨 **主机一旦把"设备描述符读取失败"缓存住就再也不重试** —— 设备管理器里是
   「未知 USB 设备(设备描述符无效)」，而固件这边其实一切正常（`cfg` 永远是 0）。
   没人拔线的话只能靠固件**自己发一次软拔插**救回来（`s31_usb_soft_reconnect()`，
   主循环里 `RECONNECT_EVERY_MS` 兜底）。
   ⚠️ 这个超时**必须明显大于主机正常枚举所需时间**（实测最慢见过 ~5 秒），
   设小了会在正常枚举到一半时把设备拔掉 —— 自己把自己打断。
2. 🚨 **跳进 App 之前必须干净地关掉 USB PHY**（`s31_usb_phy_off()`）：
   PHY 的上电位在常电域（HP_ALIVE / LP_SYS）**跨复位保留**，而 App 根本不碰 USB，
   于是主机一直看到一个不回答控制传输的僵尸设备 → 判描述符无效并缓存住 →
   之后怎么复位都识别不了。实测特征：纯 bootloader 反复复位 4/4 正常，
   **跑过一次 App 再回来就废**。
3. 🚨 **CherryUSB 的库日志是在 USB 中断里打的**，必须加"中断闸门"
   （只有主循环里才允许真输出），否则中断里同步写控制台会把 MSC 事务拖垮。

---

## 6. 目录结构

```
boot_msc_s31/
├── README.md            ★ 本篇（现状 / 上手 / 设计 / 硬件事实 / 踩坑速查 / 文档索引）
├── Makefile             ★ make help / build / flash / monitor / erase / erase-all
├── prompt.txt           本项目的**原始需求**（开工前那份任务书，四条需求 + 两个参考工程路径）
├── PSRAM.md             ★ PSRAM 专题（初始化全流程 + 两个真凶 + 证据 + 复现命令）
├── DEBUG-LOG.md         排查流水账（按轮次；大部分怀疑方向已被证伪，留着"别再走一遍"）
├── captures/            PSRAM "寄存器对差"的**原始 dump**（IDF 侧 vs 裸机侧）+ 判读说明
├── bsp/
│   ├── startup.S        上电入口：关看门狗/超级看门狗喂狗/栈/gp/mtvec=CLIC/清 bss
│   ├── trap.S           CLIC 陷阱入口（64B 对齐）+ 换 ISR 栈 + 存 caller-saved
│   ├── trap_handler.c   中断分发 + 异常现场打印（守卫可跳过探测期的异常指令）
│   ├── linker.ld        内存布局 + 栈 + ROM 函数/外设实例符号 PROVIDE + 3 条 ASSERT
│   ├── s31_regs.h       用到的寄存器地址与位（每个都标了 IDF 出处）
│   ├── s31_clk.c        SYSTIMER 时基 / 延时（延时里顺便刷日志）/ 整片复位
│   ├── s31_cpuclk.c     CPLL 40→320MHz 提频（移植自 rtt_nano_s31）
│   ├── s31_usj.c        控制台：USB-Serial/JTAG + 4KB 发送环形缓冲
│   ├── mini_libc.c      -nostdlib 下的字符串/内存 + kprintf/vkprintf + 64 位除法 + abort
│   ├── s31_flash.c      flash 读写擦（ROM legacy API + 容量修正）+ **flash 映射窗口**
│   ├── s31_psram.c      ★ PSRAM **全部**初始化 + MMU 映射 + 原始读写（单文件，~800 行）
│   ├── s31_usb_glue.c   USB 时钟/复位/PHY + FIFO 划分 + CLIC 中断
│   ├── usb_osal_bare.c  CherryUSB 裸机 OSAL（临界区 + no-op 桩）
│   ├── sdkconfig.h      极简版（只定义编译 IDF 那几个 LL 头所必需的宏，非 IDF 生成的大文件）
│   ├── s31_layout.h     ★ flash 布局 + App 镜像/描述符格式（**App 也要包含它**）
│   ├── usb_config.h     CherryUSB 配置
│   └── idf_headers/     ★ 冻结的 IDF LL 头（45 个 / 2.4MB，自动生成，见 §9）
├── app/
│   ├── main.c           启动决策（按键/描述符）+ 拖拽收尾状态机 + 开机映射 flash 窗口
│   ├── msc_disk.c       ★ 虚拟 FAT16（元数据 RAM、**数据区按"相对首簇"折算到固定地址**、4KB 写回缓存）
│   └── app_image.c      CRC32 / 镜像校验 / 描述符读写 / 搬运 + 跳转（PSRAM 段走 raw 写）
├── cherryusb/           CherryUSB 1.6.1 最小子集（core + msc + dwc2 + common）
└── tools/
    ├── build.ps1        编译→链接→elf2image→烧 0x2000→读日志
    ├── make.ps1         Makefile 背后干活的（找盘/擦除/读串口/看门狗）
    ├── sync_idf_headers.ps1  ★ 用 `gcc -M` 重新冻结 IDF 头（见 §9）
    ├── read_port.py     读串口（显式 setDTR/RTS False，UTF-8 + errors=replace）
    └── idf_compile_intel.py    把 IDF 源文件重编译成带源码注释的 .s（看它真写了什么）
```

> `bsp/` 一共 16 个自己的文件（+ `idf_headers/` 里 45 个冻结的 IDF 头）。
> **PSRAM 就在 `s31_psram.c` 一个文件里**，对外只有 5 个函数（见文件头注释）。
> 配套的 **`projects/psram_probe`**（IDF 工程）= 排查时拿来当"真值机"的寄存器基准，
> 见 [PSRAM.md](PSRAM.md) §4。

---

## 7. 文档索引 & 排查史

本工程的文件分工（**看代码之前先看这张表，能少走很多弯路**）：

| 文档 | 讲什么 | 什么时候看 |
|---|---|---|
| **README.md**（本篇） | 现状 / 上手 / **设计**（§3 内存模型）/ 硬件事实 / **踩坑速查**（§5） | 第一次接手 |
| [prompt.txt](prompt.txt) | **原始需求**（四条）+ 两个参考工程路径 —— 现在的设计就是照着它来的 | 想确认"当初到底要做成什么样" |
| [PSRAM.md](PSRAM.md) | **PSRAM 专题**：初始化全流程、两个真凶（mode register 位域 + **PMA**）、证据、复现命令 | 要碰 PSRAM 的时候 |
| [DEBUG-LOG.md](DEBUG-LOG.md) | **排查流水账**（按轮次）——大部分怀疑方向已被证伪，留着当"别再走一遍" | 想知道"当初为什么这么改" |
| [captures/](captures/) | PSRAM 寄存器对差的**原始 dump**（IDF 侧 / 裸机侧）+ 怎么自己复核 | 想验 PSRAM.md §4 的结论 |
| [../s31_app/README.md](../s31_app/README.md) | App 侧：怎么编、怎么拖、开机自检怎么读、怎么排错 | 写 App 的时候 |

**里程碑（详细过程见 DEBUG-LOG.md）**

| 里程碑 | 一句话 | 详情 |
|---|---|---|
| M1/M2 | 裸机启动 + 控制台 + flash 读写 + USB-HS 枚举成 MSC | 本篇 §1、§5 |
| M3 | PSRAM 初始化通了 —— 真凶一：mode register 位域抄错 | [PSRAM.md](PSRAM.md) §2 |
| M4 | 镜像搬运 + 跳转（描述符门禁：PSRAM 没起来就不搬 PSRAM 段） | 本篇 §3 |
| M4b | App 模板工程 + 打包工具 | [../s31_app/README.md](../s31_app/README.md) |
| M5 | "PSRAM 能不能写" —— 真凶二：**PMA**（CPU 自定义 CSR，寄存器 dump 看不见） | [PSRAM.md](PSRAM.md) §12 |
| M6 | 扁平 `objcopy` 产物直接拖 | 本篇 §3.1 |
| **M7** | **经典模型**：LMA 全在 flash、VMA 全在 PSRAM；`.text` 由 bootloader 搬，`.data` 由 App 自己从 flash 搬 | 本篇 §3.0 |

**两条常用的救场命令**（详细用法见 [DEBUG-LOG.md](DEBUG-LOG.md) 的"调试小技巧"）：

```powershell
# ① 强制回 bootloader 模式（App 有效时板子会直接跑 App、不出现 U 盘）：
#    把描述符块擦成 0xFF 就行
python -m esptool --chip esp32s31 -p COM43 --no-stub erase-region 0x900000 0x1000

# ② 板子卡住 / 串口完全没声时，用 JTAG 抓现场（比猜日志快得多）：
#    PC + ra 抓出来，再用 addr2line 反查是哪个函数
openocd -s <scripts> -f board/esp32s31-builtin.cfg `
        -c "init" -c "halt" -c "reg pc" -c "reg ra" -c "shutdown"
```

## 8. 下一步（还没做的）

| 项 | 说明 |
|---|---|
| `mkapp.py`（格式 A 容器） | 现在 `s31_app` 默认产扁平格式，容器路径没有工程在用了；留着是为了"多区域 VMA"的场合。要么给它补一个样例工程，要么删掉 |
| 多 App / 分区表 | 现在只支持"一个 App"，描述符只有一份。要并存多个得扩描述符 |
| 拖拽进度反馈 | 现在只有每 5 秒一行状态（`wr= / flash(erase= / prog= / data_hi=`），没有百分比。目标大小在描述符/目录项里是已知的，要加不难 |
| 失败回滚 | 描述符是最后一步才写的，所以"拖到一半拔线"的行为是安全的（`app_probe` 会 CRC 不匹配 → 留在 bootloader），但没有"回滚到上一个能跑的 App" |
| 把 `bsp/idf_headers/` 挪出仓库？ | 现在 2.4MB 的 IDF 头快照是**进库**的（为的是自包含）。如果哪天嫌它大，可以移到 `.gitignore` + 首次构建时自动 `sync_idf_headers.ps1` |

> ✅ **曾经记在这里的"看门狗咬了一口"是个误判**：复位原因报
> `rst:0x7 (HP_SYS_HP_WDT0_RESET)`，看着像 HP WDT0 提前触发，其实
> **`s31_system_reset()` 自己就是走 TIMG0 的 MWDT（stage0 = RESET_SYSTEM）** ——
> 这是当初特意选的复位路径（见 `s31_clk.c` 里那段注释：core 软复位会把自己永久按死、
> RTC_WDT 因为没配慢时钟域根本不走）。所以那个复位原因正是**我们主动复位**的指纹，
> 一切正常，不需要额外喂狗。

---

## 9. 自包含程度（哪些在工程内、哪些是外部的）

**✅ 两个工程现在都是 100% 自包含的** —— clone 下来只要有 RISC-V 工具链就能编，
**不需要装 ESP-IDF**。

| | `boot_msc_s31` | `s31_app` |
|---|---|---|
| 自己的源码（`bsp/` `app/`） | ✅ 全部在工程内 | ✅ 全部在工程内 |
| CherryUSB | ✅ vendored 在 `cherryusb/`（1.6.1 子集） | ➖ 不用 |
| `sdkconfig.h` | ✅ 手写的极简版，在 `bsp/` | ➖ 不用 |
| 编译期头文件 | ✅ **IDF 的 LL 头已冻结在 `bsp/idf_headers/`**（45 个 / 2.4MB） | ✅ 只有工程内的 `s31_regs.h` |
| 链接期 | ✅ `-nostdlib`，不链 IDF、不链 newlib | ✅ 同左 |
| 工具链 | RISC-V GCC（`~\.espressif\tools\riscv32-esp-elf`） | 同左 |
| 烧录 / 读日志 | python + esptool（`-BuildOnly` 之外才用） | ✅ 不用 python |

### 9.1 为什么有 `bsp/idf_headers/` 这么个东西

`bsp/s31_psram.c` 要用 IDF 的 4 个 LL 头（`hal/psram_ctrlr_ll.h`、`hal/mspi_ll.h`、
`hal/mmu_ll.h`、`soc/cache_reg.h`）——**按字段名写寄存器，不手抄位号**。
本项目因为手抄位号栽过两次：`MSPI_DIV.FB_DIV` 实际在 bit[7:3] 而我写在 bit[4:0]；
AP PSRAM 的 MR0/MR4/MR8 三个位域全凭印象拍错。两次都是"寄存器写错、不报错、
只是行为不对"，各花掉一整轮。完整记录见 [PSRAM.md](PSRAM.md) §2。

那 4 个头会拖出一整个传递闭包（大头是 `soc/spi_mem_*_struct.h` 这种把每个寄存器
都列一遍的文件），所以把它们**冻结进工程**：

```powershell
pwsh -File tools\sync_idf_headers.ps1            # 重新同步（覆盖 bsp\idf_headers\）
pwsh -File tools\sync_idf_headers.ps1 -WhatIf    # 只看会拷哪些
```

- 脚本**不自己解析 `#include`**（会漏掉条件编译、以及 `#include "sibling.h"` 这种
  靠"同级目录"解析的形式），而是用 **`gcc -M` 问编译器**：依赖表里所有落在 IDF 树
  里的头全拷过来。**编译器读到的就是全部**，不会漏。
- 目录形状 = 每个头"相对命中它的那个 include 根"的路径（include 字符串本身就是
  将来在扁平目录下的相对路径）；顺序 = 编译器搜索 `-I` 的顺序。
- 来源记录在 [`bsp/idf_headers/_SOURCE.txt`](bsp/idf_headers/_SOURCE.txt)
  （IDF 版本 `v6.1.0` / commit `fff9895c8` / 同步时间），说明见
  [`bsp/idf_headers/README.md`](bsp/idf_headers/README.md)。

`build.ps1` 里现在只剩**一行** `-I$IdfHdrs`（放在最后，让工程自己的同名文件优先命中），
再没有任何 IDF 路径 —— 想验证这一点，看 `build.ps1` 里 grep 不到 `esp-idf` 即可。

**什么时候需要重新同步**：只有当 IDF 那边 **S31 的寄存器定义被修正**时（比如 S31
从 preview target 转正）。跑一次脚本 → 重编 → 上板验证。

### 9.2 顺手修掉的"产物不可复现"

验证自包含的时候，我把工程**拷到另一个目录**编译了一遍，发现 `app.bin` 从 41104
变成 41184 字节 —— 追下去发现 **CherryUSB 有几处把 `__FILE__` 编进了字符串**
（assert / 日志），绝对路径一变，`.rodata` 就跟着变（差 84 字节 = 路径长度差）。

虽然不影响功能，但"换个目录编译出来的镜像就不是同一份"很讨厌（也没必要把本机路径
带进固件）。所以加了：

```powershell
"-ffile-prefix-map=$Root=."     # 绝对路径 -> 相对路径
```

现在**同一份源码在任何目录编译出来的 `.text` 都是逐字节相同的**，
`app.bin` 之间只剩 `__DATE__`/`__TIME__`（构建时间戳）和 esptool 镜像头里的
SHA256 会变 —— 实测两个不同目录的产物：大小都是 41040，只有 35 字节不同（时间戳+校验和）。
