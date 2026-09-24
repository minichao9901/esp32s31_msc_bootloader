# s31_app —— ESP32-S31 裸机 App 模板（拖进 S31-BOOT 盘就能跑）

> 配套的 bootloader 在 `projects/boot_msc_s31`。**内存模型、为什么这么设计、
> 踩过的坑，全在 `boot_msc_s31/README.md` §3** —— 这里只讲"怎么用 + 怎么验收"。

## 快速上手

```powershell
cd s31_app
make help          # 目标一览
make build         # 编译 -> build\app.bin + build\app_container.bin（**两个都产**）
make flash         # ★ 全自动：擦描述符 -> 等 S31-BOOT 盘 -> 编译 -> 拷进去
make drag          # 只拷贝（板子要已经在 bootloader 模式）
make monitor       # 一直看串口（Ctrl+C 退出；只看一段：make monitor SECONDS=8）
make run           # flash 然后 monitor
make erase         # 擦掉 App 描述符 -> 板子回到 bootloader 模式
```

> `make` 只是薄封装，真正干活的是 `tools\build.ps1`（编译+打包）和
> `tools\make.ps1`（找盘/等盘/擦除/读串口）。不想用 make 就直接调它们。
> 端口取环境变量 `ESP32_S31_PORT`（没设就用 `COM43`；也可以临时 `make monitor PORT=COM7`）。

### 两个镜像，都能拖（`make build` 一次产两个）

| 文件 | 格式 | bootloader 怎么处理 |
|---|---|---|
| **`build\app.bin`**（默认拖这个）| 扁平 `objcopy -O binary` 产物 | 整块拷到 `0x50000000`，然后跳入口 |
| `build\app_container.bin` | 格式 A：64B 头 + 8×16B 段表 + payload | **按段表逐个搬**：`.image`→`0x50000000`、`.data/.sdata`→`0x50800000`、`.bss` 清零 |

两者**共用同一份源码**，但**链接基准不同**（这是关键）：

```
app.bin            .image LMA = 0x40100000      ← flash 0x100000 的别名
app_container.bin  .image LMA = 0x401000C0      ← 多出 192 字节头+段表，基准跟着挪
```

为什么必须挪：App 的 `startup.S` 是按「**文件内偏移 == flash 装载偏移**」去读 `.data`
初值的（`la t0, __data_lma`）。容器前面有 192 字节头，payload 整体后移，基准不跟着挪的话
读到的正好是那张段表 —— 实测 `magic` 读成 `0x00000000`。

想拖容器那个：`make drag BIN=app_container.bin`（或 `make flash BIN=app_container.bin`）。

> 日常用 `app.bin` 就行 —— 容器格式是留给"App 的运行位置不止一处、扁平表达不了"
> 的场合（那种情况下才需要段表）。两种都真板验过。

> ✅ **自包含**：源码、头文件、构建脚本全在工程内，编译只靠 **RISC-V GCC**
> （`~\.espressif\tools\riscv32-esp-elf`）—— **不需要装 ESP-IDF**，也不用 python 编译；
> `-nostdlib` 不链 newlib，需要的那点 libc（`kprintf`/延时）是自己写的。
> 唯一"外来"的是 `bsp/idf_headers/` 里**冻结的 12 个 IDF v6.1 头**（RMT 的 include 链）——
> 冻结而不是依赖 IDF 树，理由和做法见 `bsp/idf_headers/README.md`。

### 不用 make 的原始流程（等价）

```powershell
pwsh -File tools\build.ps1                        # 编译 -> app.bin + app_container.bin
pwsh -File tools\build.ps1 -Drag                  # 编译完把 app.bin 拷进 S31-BOOT 盘
pwsh -File tools\build.ps1 -Drag -Bin app_container.bin   # 拷容器那个
pwsh -File tools\build.ps1 -Rebuild               # 全量重编
```

### 三个"守门检查"（改坏了直接编译失败，别绕过）

1. **扁平镜像大小 == 各装载段 LMA 总跨度** —— 保证「文件内偏移 == 装载偏移」
   （这条是拿真事换来的：`.clic_entry` 差 62 字节 → `.text` 整体错位 → 必崩）；
2. **容器构建的 `.image` LMA 必须是 `0x401000C0`** —— 保证 `--defsym` 生效了；
3. **容器去掉 192 字节头之后 == 扁平镜像** —— 保证 payload 保持了 LMA 布局
   （`.image` 与 `.data` 之间有 2 字节对齐空隙，mkapp 一旦紧拼，App 读 `.data`
   就整体错 2 字节：`0x1234abcd` → `0x00001234`）。

拿到板子上：

1. **不按任何键**上电 → 有 App 就直接跑；没有就进 bootloader 模式；
   **按住 GPIO0（J2-9 接 GND）上电 → 强制进 bootloader 模式**（App 跑飞时的救命稻草）。
2. bootloader 模式下面板会出现 `S31-BOOT` 盘（本机是 `Z:`）。
3. 把 `build\app.bin` **拖进去**。不要拔线、不要再碰盘。
4. 停手约 1.5 秒后 bootloader 自己校验、写描述符、复位、跳进 App。

## 内存模型（一句话版）

```
LMA（初值在哪儿）                VMA（跑在哪儿）
flash 0x100000 → 别名 0x40100000   PSRAM
  .boot/.text/.rodata  ─────────→ 0x50000000   ← bootloader 整块搬
  .data/.sdata 的初值  ─────────→ 0x50800000   ← App 自己的 startup.S 搬
                                   .bss/.sbss   ← App 自己清零
                                   栈顶 0x51000000
```

**bootloader 不需要懂段表**：它只把整块 bin 拷到 `0x50000000` 然后跳入口。
`.data` 怎么来、`.bss` 怎么清，都是 App 自己的事（`bsp/startup.S`）。

## `app.bin` 是怎么来的：就是 `objcopy -O binary`

```powershell
riscv32-esp-elf-objcopy -O binary build\app.elf build\app.bin
```

**没有自定义头、没有段表、没有后处理脚本** —— `tools/build.ps1` 里就是这一条命令。
`build\app.bin` 直接拖。

之所以能这样，是因为链接脚本把"两处 VMA"摆成了 objcopy 能表达的形状：

```ld
MEMORY {
  FLASH (rwx) : ORIGIN = 0x40100000, LENGTH = 0x00800000   /* LMA：flash 的映射别名 */
  IMG   (rwx) : ORIGIN = 0x50000000, LENGTH = 0x00800000   /* VMA：代码/常量 */
  DATA  (rwx) : ORIGIN = 0x50800000, LENGTH = 0x00800000   /* VMA：可写数据 */
}
.image : { .boot / .clic_entry / .text / .rodata } > IMG  AT> FLASH
.data  : { ... }                                   > DATA AT> FLASH
.sdata : { ... }                                   > DATA AT> FLASH
.bss   : { ... }                                   > DATA          /* NOLOAD */
```

`objcopy` 按 **LMA** 排字节，所以输出是
`[.image 的初值][.data 初值][.sdata 初值]` 一条连续的流 —— 全部落在 flash 装载区。
整块拷到 `0x50000000` 之后，**前半段（入口/代码/常量）偏移天然对齐**、原地就能跑；
后半段（变量初值）被拷到 `0x50001xxx` 这种没人用的地方（无害），
真正生效的是 `startup.S` 从 flash 那次搬运。

> 🚨 唯一的硬约束：**`.image` 必须是一个输出段**。
> 按段分别写 `> IMG AT> FLASH` 的话，ld 会给各段紧凑分配 LMA、**不镜像 VMA 侧的
> 对齐空隙**，扁平镜像就和 VMA 错位了。`tools/build.ps1` 里有守门检查
> （`app.bin` 大小必须 == 各段 LMA 总跨度），改坏了直接编译失败。

## 开机自检（每次上电只打一屏，可以直接当验收）

```
================================================
  S31 bare-metal App is ALIVE
  build Aug 25 2026 12:00:00
------------------------------------------------
[app] .image VMA 50000000 <- LMA 40100000   [bootloader 搬]
[app] .data  VMA 50800000 <- LMA 40101258   [startup 从 flash 搬]
[app] 布局自检: OK —— LMA 在 flash、VMA 在 PSRAM，初值搬运与 .bss 清零都对
[app] LED: RMT -> WS2812 (GPIO60)，红/绿/蓝 1 秒轮换
================================================
```

**打完这一屏串口就安静了 —— 之后板载 LED 就是心跳**（红/绿/蓝每 1 秒轮换）。
`布局自检: OK` 的判据（`main.c` 的 `layout_ok()`，四条全过才 OK）：

| 判据 | 说明 |
|---|---|
| `__image_lma` 落在 `0x40000000..0x50000000` | 装载地址在 flash 映射窗口里（初值真在 flash）|
| `&s_magic >= 0x50800000` | 变量真的落在 PSRAM 的 DATA 区 |
| `s_magic == 0x1234abcd` | **`.sdata` 从 LMA 搬运成功** |
| `s_data_blob[0] == 0x0badf00d` 且 `s_counter == 0` | **`.data` 搬运成功 + `.bss` 清零** |

- `s_magic`（4 字节）在 **`.sdata`**，`s_data_blob[64]`（256 字节）在 **`.data`** ——
  两条搬运路径都覆盖到了。
  ⚠️ `s_data_blob` 必须 `volatile`：否则 `-O2` 会把读取折成常量、把数组整个删掉，
  `.data` 就永远是 0 字节（踩过）。
- `.image` 的 LMA 具体值随镜像格式变（扁平 `40100000` / 容器 `401000C0`），
  所以那行**打印符号值**、不写死常数。

## 板载 RGB LED（WS2812 @ GPIO60，RMT 驱动）

主循环拿它当心跳。**别改成 CPU 翻转引脚** —— 这个灯坑过三次，三次都是
"灯不对、但没有任何报错"：

| 版本 | 做法 | 现象 |
|---|---|---|
| v1 | `s31_delay_us()` 做位延时 | 粒度 1µs 比整个位周期（1.2µs）还长 → 每位都采成 1 → **常亮白** |
| v2 | mcycle 忙等 | GPIO 读改写的开销叠进位周期 → 慢 1.9 倍 |
| v3 | RMT，但符号写进 `chndata`（FIFO 路径）| TX 引擎收不到数据 → **一帧只花 5.3µs**，灯毫无反应 |
| **v4** | RMT + `apb_fifo_mask=1` + 写 **RMTMEM** | 正常（一帧 79.8µs ≈ 理论 78.8）|

三条硬结论（依据和出处都在 `bsp/s31_rmt.c` 的文件头）：

1. **符号要写进 `RMTMEM`（`0x20355800`），不是 `RMT.chndata`** —— IDF 走的是
   `apb_fifo_mask=1` 的非 FIFO 直连内存那条路（`rmt_hal.c:15` + `rmt_tx.c:352`）。
   FIFO 路径发不出数据，而且**症状极像"时基/CPU 主频不对"**（一帧几微秒就"发完"）。
2. 符号用 IDF 的 `rmt_symbol_word_t` **按字段名**填，不手拼位域。
3. 等 `TX_DONE` 必须带超时；`send()` 还会**写完回读比对**（不一致返回 `-2`）——
   "符号进没进内存"从灯上看不出来，只能读回来。

位时序照抄 IDF 例程 `projects/led_rgb`：T0H=0.3µs / T0L=0.9µs、T1H=0.9µs / T1L=0.3µs、
reset=50µs；时基 = XTAL 40MHz ÷1(组) ÷4(通道) = 10MHz。
⚠️ RMT 的位时序**与 CPU 主频无关**（源在 `HP_SYS_CLKRST.rmt_ctrl0` 里选）；
CPU 主频本身由 bootloader 提到 320MHz，App 侧不用管。

## 文件

| 文件 | 作用 |
|---|---|
| `Makefile` | `make help / build / flash / drag / monitor / run / erase` |
| `bsp/linker.ld` | ★ 三个 MEMORY 区 + `.image` 一个输出段 —— 整个模型的形状都在这儿 |
| `bsp/startup.S` | ★ `_start`：换栈 → 装 gp → 设 CLIC mtvec → **从 LMA(flash) 搬 `.data`/`.sdata`** → 清 `.bss` → `call app_main` |
| `bsp/s31_regs.h` | 寄存器地址/位域宏 |
| `bsp/mini_libc.c` | `kprintf`（走 USB-Serial/JTAG）+ `delay`，无 libc |
| `bsp/s31_clk.c` | SYSTIMER 时基 / `s31_millis` / 延时 / 整片复位 |
| `bsp/s31_usj.c` | 控制台硬件初始化 / 泵 |
| `bsp/s31_rmt.c` | ★ 裸机 RMT 驱动 WS2812（**三次踩坑的经验都在文件头**）|
| `bsp/trap.S` + `bsp/trap_handler.c` | CLIC 陷阱入口 + 异常打印（`mcause`/`mtval`/PC）+ **`s31_irq_install()`** |
| `bsp/sdkconfig.h` | 极简的一份，只为让 IDF 头能编过（`CONFIG_HAL_DEFAULT_ASSERTION_LEVEL`）|
| `bsp/idf_headers/` | **冻结**的 12 个 IDF v6.1 头（RMT 的 include 链），见其中的 README |
| `app/main.c` | ★ 样板：段布局自检一屏 + LED 心跳 |
| `tools/build.ps1` | 编译 + `objcopy`（扁平）+ `mkapp.py`（容器）+ 三个守门检查 |
| `tools/make.ps1` | Makefile 背后干活的（找盘/等盘/擦描述符/读串口）|
| `tools/read_port.py` | 读串口（显式 setDTR/RTS False，UTF-8 + errors=replace）|
| `tools/mkapp.py` | 打**格式 A（带段表容器）**的镜像 —— `app_container.bin` 就是它产的 |
| `tools/sync_idf_headers.ps1` | 重新冻结 `bsp/idf_headers/`（用 `gcc -M` 问编译器要 include 闭包）|

> 📌 本工程**没有**自己的 `s31_layout.h` —— flash 布局常量（`S31_APP_LOAD_BASE`、
> `S31_APP_FLAT_VMA`、镜像/描述符格式）只有**一份**，在
> `../boot_msc_s31/bsp/s31_layout.h`。App 侧唯一需要跟着它改的地方是
> `bsp/linker.ld` 里 `ORIGIN(FLASH)` 那个数字（有注释标着）。
> （曾经复制过一份过来，结果两边悄悄跑偏了 —— 所以删掉了。）

### 模板自带、但 `main.c` 暂时没用到的 API

这些是**有意留着的接口**（不是死代码），要用直接调：

| API | 用途 |
|---|---|
| `s31_irq_install(id, fn, arg)` | 注册 CLIC 中断处理（`id` 见 `trap_handler.c` 的说明） |
| `s31_system_reset()` | 整片复位（走 TIMG0 的 MWDT；**别用 core 软复位**，那条路会把自己按死） |
| `s31_usj_getc()` | 从控制台读一个字节（没有就返回 -1） |
| `s31_usj_flush(guard)` | 等发送缓冲排空（带超时；复位前想确保日志发完时用） |
| `s31_delay_us(us)` | 微秒级忙等（里面会顺手刷控制台，卡住时也看得到日志） |

## 换个地址怎么办

- **改 App 装载区**：动 `boot_msc_s31/bsp/s31_layout.h` 的 `S31_APP_LOAD_BASE`
  （同时 `S31_APP_LMA_BASE` 跟着变），**然后**把本工程 `bsp/linker.ld` 里
  `ORIGIN(FLASH)` 改成同一个值、重编重拖。两边是**耦合**的，改一处必须改另一处。
- **改 PSRAM 里代码/数据的分界**：改 `linker.ld` 的 `IMG` / `DATA` 两个 ORIGIN 即可，
  bootloader 不用动（它只认 `S31_APP_FLAT_VMA` 和整块拷贝）。

## 排错

| 现象 | 多半是 |
|---|---|
| 拖进去之后串口完全没声 | 描述符说 READY 但镜像其实不对 —— 现在 `app_probe()` 会比对 `img_crc`，不匹配会明确打 `image CRC mismatch` 并留在 bootloader |
| `[app] image CRC mismatch` | flash `0x100000` 里的内容和描述符对不上（拖了一半 / 上一次的残留）→ 重新拖一次 |
| App 跑飞 | **按住 GPIO0（J2-9 接 GND）上电**强制进 bootloader 模式，重新拖 |
| 板载灯不亮 / 常亮白 / 颜色乱 | 看 `bsp/s31_rmt.c` 的三条硬结论；串口会打 `WS2812 发送失败 rc=` |
| 编译时守门检查报错 | `linker.ld` 里 `.image` 被拆开了，或者引入了新的输出段 —— 见上面"唯一的硬约束" |
| `[mmu] ...` 没出现 | bootloader 太老，是改这一版之前的固件 |
