# DEBUG-LOG —— 排查流水账（按轮次）

> **这是什么**：本工程从"PSRAM 参考字 MISMATCH"一路查到"App 能从 PSRAM 跑起来"的
> **过程记录**，按轮次排。里面**大部分怀疑方向已经被证伪**（这正是留下来的价值 ——
> 省得下一个人再走一遍），所以：
>
> - 🎯 **要看结论 → 看 [PSRAM.md](PSRAM.md)**（PSRAM 专题，去掉了弯路，只留结论+证据）
> - 🎯 **要看设计 → 看 [README.md](README.md)**（§3 内存模型 / §5 踩坑速查）
> - 📜 这篇只在"想搞清楚当初为什么这么改"的时候翻。
>
> PSRAM 那几轮（第 6~9 轮）与 PSRAM.md 有重叠，**以 PSRAM.md 为准**；
> USB 稳定性、flash 容量、控制台日志那几段是这篇独有的。

## 目录

| 轮次 | 主题 | 一句话结论 |
|---|---|---|
| 第 4 轮 | 改用 IDF 原版 LL 函数 | 别再手抄位号 |
| 第 5 轮 | ⭐ MSPI_DIV.FB_DIV 在 bit[7:3] | 分频写错 → PSRAM 初始化卡死 |
| 第 5 轮补充 | 两个"看着吓人其实正常"的现象 | |
| 第 5 轮补充 2 | 串口助手中文乱码 | 别改固件，改终端 |
| 第 5 轮补充 3 | 🚨 主机缓存"枚举失败" | 固件自己发软拔插救回来 |
| 第 5 轮补充 4 | 🚨 库日志打在 USB 中断里 | 必须加中断闸门 |
| 第 6 轮 | ⭐ PSRAM 通了（mode register 位域） | 详见 PSRAM.md §2 |
| 第 7 轮 | ⭐ App 从 PSRAM 跑起来了 | 详见 PSRAM.md §10 |
| 第 8 轮 | PSRAM 整理成一个文件 | 详见 PSRAM.md §11 |
| 第 9 轮 | ★★★ 真凶是 PMA | 详见 PSRAM.md §12 |
| 调试小技巧 | 擦描述符 / OpenCCD dump 寄存器 | 两条常用命令 |
| M4 / M4b | 搬运+跳转 / App 模板 | 现在的设计见 README §3 |

---
### M3 PSRAM 初始化 —— ✅ **第 6 轮做完了，完整记录见 [PSRAM.md](PSRAM.md)**

下面保留 M3 前 5 轮的过程记录（**里面的怀疑方向大多已被证伪**，
不要再照着查；真正的结论在 [PSRAM.md](PSRAM.md)）：

> ⚠️ **先看这一段的结论**：真凶是 **mode register 的位域**（MR0 的 `read_latency` 在 bit[4:2]、
> `lt` 在 bit[5]；MR4 的 `wr_latency` 在 bit[7:5]；MR8 是 `bl[1:0]/bt[2]/rbx[3]/x16[6]`），
> 跟 **dummy 长度 / 采样点 / timing tuning / 分频 / DQS 延迟线** 全都没关系。
> 下面是当时走过的弯路，留着当"别再走一遍"的记录。

IDF 里 `esp_psram_init()` 是 **app 阶段**才调的（`cpu_start.c:659`），bootROM 不做，
所以裸机得自己点。现在 `bsp/s31_psram.c` 已经做完并**逐条验证过**的部分：

| 环节 | 状态 | 证据 |
|---|---|---|
| MPLL 500MHz | ✅ | `fb_div=24 cal_end=1`（与 IDF 系统 dump 出的 `MSPI_DIV=0x298` 一致）|
| 控制器时钟/复位/选源 | ✅ | `psram_ctrl0=0x1a1`，与 IDF 系统 dump **逐位一致** |
| 焊盘 DRV/IE/HYS/DQS | ✅ | 与 dump 值一致（DRV 在 **bit[14:12]**，不是 [2:0]）|
| CS 时序/页大小/split | ✅ | `smem_ac`/`ecc_ctrl` 与 dump 一致（注意寄存器里存的是 **n-1**）|
| SPIMEM2 配成 octal DDR + AXI | ✅ | `cache_sctrl=0x02f7c679` 等与 dump 一致 |
| 总线分频 | ✅ | SPIMEM2 分频 **4** / SPIMEM3 分频 **2**（照 dump，不是照代码）|
| ROM MMU 映射 | ✅ | `Cache_PSRAM_MMU_Init()` + `Cache_PSRAM_MMU_Set(0,0x50000000,0,64,256,0)` → rc=0 |
| **DDR 读采样点** | ❌ | 回读恒 **0xAAAAAAAA**（每 bit 被采两遍 = 采样点完全错位）|

**已排除的**：分频不对（2/4 都试过）、走 SPIMEM2 做事务（返回全 0，更差）、
DQS 焊盘 `DELAY_90/DELAY_270` 两维扫描 16×16（无一命中）。

**第 3 轮又缩小了范围**（都是真板实测）：

- 找到了正确的 tuning 旋钮（读 `mspi_timing_by_dqs.c` + `hal/mspi_ll.h`）：
  ① DQS 相位 `PSRAM_DQS_0_PIN0.PHASE`（bit[2:1]，4 档）；
  ② 延迟线：数据/CLK/CS 动各自焊盘的 **DLC**（bit[7:4]），DQS 动 `DELAY_90`(bit[10:7])
     与 `DELAY_270`(bit[21:18]) 且**两者必须同值**。IDF 扫 31 组 {dqs,data}。
  → 4 档相位 + 31 组延迟线**全部失败**。
- **把"事务之后"的 SPIMEM3 寄存器与 IDF 正常系统逐字对比 → 关键寄存器全对上**
  （`+0x08/0x0c/0x14/0x18/0x1c/0x20/0x3c` 完全一致），说明 ROM 的
  `esp_rom_spi_set_op_mode(3, OPI_DTR)` 那一侧配置是对的。只剩
  `+0x04`(0x80 vs 0xc0)、`+0x34`(0x02 vs 0x1ff)、`+0xd4`(0x21 vs 0x23，bit1=`FMEM_VAR_DUMMY`)。
- 走 SPIMEM2 做事务会得到**全 0**（比 0xAAAAAAAA 更差）→ 反证 SPI3 才是通的那条路，
  芯片**是有响应的**。

**两个必须记住的教训**：

1. 🚨 **mode register 是"掉电才清"的，读失败时千万别盲写**。
   早期版本不管读回来是什么都改一改写回去 → 读失败时（0xAAAA）等于往芯片里灌垃圾配置，
   把芯片自己的配置搞坏，而且 IDF 也救不回来（只能拔电）。现在 `psram_init_mode_reg()`
   只在 vendor id 读出 0x0d/0x1a 时才写。
2. 🚨 **`LP_*`/`LP_AON` 寄存器跨复位保留 → 跑过 IDF 固件之后，本工程的 MPLL 初始化就不灵了**。
   实测：先烧 `led_rgb`（IDF 工程，会用 CPLL/MPLL 配时钟）、复位后再跑本工程，
   `cal_end` 恒为 0（MPLL 标定超时）；而在此之前一直是 1。
   本工程的时钟初始化**假设了 bootROM 交棒时的状态**，这个假设不成立。
   要做成产品级，得照 IDF 的 `clk_ll_mpll_disable()/enable()` 走一遍完整开关，
   而不是直接清 `CAL_STOP` 等 `CAL_END`。

### 调试小技巧（这轮很省时间）
- **远程强制回 bootloader 模式**：App 有效时板子会直接跑 App、U 盘不出现。
  擦掉描述符块就行：
  ```powershell
  python -m esptool --chip esp32s31 -p COM43 --no-stub --before default-reset --after hard-reset `
      erase-region 0x900000 0x1000
  ```
- **参照系**：把带 PSRAM 的 IDF 工程（`led_rgb`）烧进去，用 OpenOCD dump 寄存器：
  ```powershell
  openocd -s <scripts> -f board/esp32s31-builtin.cfg -c "init" -c "halt" -c "mdw 0x20502000 24" -c shutdown
  ```

**症状的正确读法**：回读 `0xAAAAAAAA` 而不是全 `0xFF`/全 `0x00`，
说明**信号是通的、只是采样相位错**——不是没接上、也不是控制器没时钟。

**下一步该查什么（留给下一轮）**：
1. IDF 的 `mspi_timing_psram_tuning()` 到 s31 走的是 `tuning_scheme_impl/mspi_timing_by_dqs.c`
   （`esp_hw_support/mspi/mspi_timing_tuning/`）。要看清楚它到底扫哪个寄存器：
   候选是 `SPIMEM2 +0x180/0x190`（timing_cali）、`smem_ddr.smem_usr_ddr_dqs_thd[20:14]`、
   或者 SPI1/SPI0 的 `DLL` 延时寄存器。
2. 参考值可以再从正在跑的 IDF 工程里 dump：**同一块板两次 dump 出来的 DQS 焊盘值不一样**
   （一次 `0x00020000`、一次 `0x00050081`），说明那确实是 tuning 的产物。
3. 另一个没排除的：SPI3（做 mode register 事务的那个实例）的焊盘路由。
   IDF 调 `esp_rom_spi_set_op_mode(3, OPI_DTR)`，但 ROM 里 **没有导出**
   `esp_rom_opiflash_pin_config`（S31 的 rom.ld 里没有），所以那一侧的焊盘路由可能没做上。

### M4 搬运 + 跳转（等 M3）
读描述符 → 校验 → 按段表把 flash 里的段搬到 PSRAM/内部 RAM → 关中断、清 mscratch/mtvec → 跳 `entry`。
现在的门禁是：**只有 `s31_psram_init()` 返回 0 才允许搬 PSRAM 段**，
否则往 `0x50000000` 写会立刻 store access fault（实测 `mcause=0x30000007, mtval=0x50000000`）。

### M4b App 模板
`projects/s31_app`（自己的 startup/linker，`.text/.rodata/.data` 在 PSRAM）
+ `tools/mkapp.py`（把 ELF 打成带段表头的 app.bin）。


### 第 4 轮：改成**直接用 IDF 的原版 LL 函数**（不再手抄寄存器）

手抄了两轮还是差一口气，说明"抄写"本身就是错误来源。所以把 IDF 的
hal/psram_ctrlr_ll.h + hal/mspi_ll.h **原样搬进 sp/idf_port/**，
再补几个 shim（hal/assert.h、hal/misc.h、hal/config.h、sys/param.h、
sp_rom_sys.h、om/opi_flash.h、soc/clk_tree_defs.h），
然后由 sp/s31_psram_ll.c **逐条照 sp_psram_impl_enable() 调一遍真 LL 函数**。

- ✅ **这套头文件在裸机工程里编译通过了**（连 spi_mem_s_struct.h / mspi_ll.h 一起），
  说明"把 IDF 的 LL 拿来用"这条路是通的 —— 以后再遇到"寄存器抄不对"就该这么干。
- ⚠️ 需要给 linker.ld 加一批**外设实例符号**（SPIMEM2/SPIMEM3/MSPI_IOMUX/HP_SYSTEM/
  HP_SYS_CLKRST/HP_ALIVE_SYS/LP_SYS/PMU）：IDF 的 LL 靠这些 xtern xxx_dev_t 实例访问寄存器，
  IDF 自己是用 peripherals.ld 提供地址的，我们照抄一份。
- ⚠️ psram_ctrlr_ll_enable_module_clock() 之类是**宏**，展开成
  do { (void)__DECLARE_RCC_ATOMIC_ENV; ... } —— 所以这个符号必须定义成**表达式**
  （写成 int x = 0; 会编译不过）。

**当前真正的拦路虎变成了 MPLL**：cal_end 恒为 0（标定超时）→ PSRAM 控制器没时钟 →
回读全 0。而这个问题是**跑过 IDF 固件之后才出现的**（见上面第 2 条教训：LP/AON 跨复位保留）。
→ **下一步：给板子做一次真正的断电重上电**，让 LP/AON 回到上电默认值，再验证
LL 版初始化；同时把 MPLL 初始化改成"不怕残留状态"的完整开关。

---

### 第 5 轮：⭐ 找到真凶 —— `MSPI_DIV.FB_DIV` 在 **bit[7:3]**，我写成了 bit[4:0]

**结论先行：PSRAM 事务第一次真的通了**（`参考字回读 OK`），根因是一个位域写错。

```
LP_AONCLKRST_MSPI_DIV_REG (0x20701054) 的真实布局：
    REF_DIV bits[2:0]（默认 1）
    FB_DIV  bits[7:3]（默认 24）   ← 分频比
    CHGP_DCUR bit[8]
```
我原来的代码是 `v = (v & ~0x1F) | fb_div;`（当成 bit[4:0]），后果有两个，
而且**都让它静默跑偏**：
- `REF_DIV` 被清成 **0**（分频比从 /2 变成 /1）；
- `FB_DIV` 只改到低 2 位：24(0b11000) → 27(0b11011)。

于是 MPLL 被要求跑 `(27+1)*40/1 = 1120 MHz` —— **自校准当然永远收敛不了**，
`MSPI_CAL_END` 恒 0，PSRAM 控制器拿不到时钟。这解释了过去三轮所有"读回全 0 / 0xAAAAAAAA"。

修完之后的实测（真板）：
| 寄存器 | 我的固件（修好后） | IDF 正常态 | 结论 |
|---|---|---|---|
| `0x20701054` MSPI_DIV | `0x299`（ref=1 fb=19） | `0x299` | ✅ 完全一致 |
| `0x20587174` ANA_PLL_CTRL0 | `0x145`（MSPI_END=1） | `0x34d`（MSPI_END=1, STOP=1） | ✅ END 都对上了 |
| `0x20704218` ext_ldo_ctrl | `0x1aa` | `0x1aa` | ✅ 1.8V 配置一致 |
| `0x207041e8` psram_cfg | `0x80000000` | `0x80000000` | ✅ xpd 都是 1 |
| 参考字回读 | **OK** | OK | ✅ **第一次通过** |

**顺带纠正两个我原先猜错的事实**（都是实测 + IDF 汇编对着看出来的）：
1. **MPLL 要 400 MHz，不是 500 MHz**。PSRAM 跑 200 MHz 是靠 `MPLL/2` 分频，
   500/200 除不尽。IDF 汇编里 `clk_ll_mpll_get_freq_mhz()` 就是
   `xtal * (fb_div+1) / 2` → `40*(19+1)/2 = 400 MHz`。
2. **PSRAM 专用 1.8V LDO 不用我开** —— 上电/ROM 之后 `PSRAM_XPD` 已经是 1，
   `ext_ldo_ctrl` 我也配成和 IDF 一样的 `0x1aa`（dref=6 mul=5）。这条不是坑，但值得记下来。

### 🔧 方法论：把 IDF 的源文件重编译成 `.s`，看它**真正**写了什么

这是本轮最值钱的一招（用户提的思路，实测极省时间）：IDF 的 LL 层全是 `static inline` + 宏，
**读 `.i`（只预处理）看不到它们到底写哪个寄存器**，要到代码生成阶段才展开成 load/store。
所以正解是 **`.s`（汇编）**，而且 IDF 的工程里就有现成的编译命令：

```powershell
# 复用 IDF 自己的 compile_commands.json，把某个源文件原样重编译成汇编
python tools\idf_compile_intel.py -B E:\esp-idf-s31\projects\led_rgb\build -m esp_psram_impl_ap_oct -k s
# -> build\intel\esp_psram_impl_ap_oct.s（每条指令前带 "源文件:行号: 源码原文" 注释）
python tools\idf_compile_intel.py -B <build> -l        # 列出所有可编译的源文件
```

- 🚨 **不要自己切分命令行**：IDF 的 `-D` 里带 `\"` 转义引号（`-DIDF_VER=\"v6.1-dirty\"`），
  自己 split 会把引号切错、后面参数全部错位（症状极具迷惑性：`stdlib.h` 报
  `unknown type name '_mbstate_t'`）。只在**原始字符串**上做定点替换、其余原样交给 cmd.exe。
- 还要补 `-Wno-error -w`（IDF 带 `-Werror`，重放编译不该被警告拦住）。
- 生成物在 `build/intel/*.s`；汇编里的 `# 文件:行: 源码` 注释让它比源码还好读。

### 第 5 轮剩下的问题：写完 mode register 后回读 MISMATCH

现在的状态是**有条件可用**：`MSPI_DIV` 对了、参考字回读通了，但把 `MR0/MR4/MR8`
写下去之后回读就 MISMATCH，于是 `s31_psram_ll_init()` 直接返回 -1（fail-safe，
见 `5.x`：**PSRAM 失败绝不能让 bootloader 起不来** —— 日志停在 `density` 那行、
USB 永不枚举、`S31-BOOT` 盘消失，就是没加这道闸时的现象）。

**下一轮要抄的东西已经从 IDF 汇编里定位好了**（`build/intel/esp_psram_impl_ap_oct.s`）：
- `esp_psram_impl_ap_oct.c:159-220` = mode register 的**构造**（mr0.lt / mr0.read_latency /
  mr0.drive_str / mr4.wr_latency / mr8.bt / mr8.bl / mr8.rbx 逐字段赋值）；
- `:313` / `:328` = **解析回读**（`read_latency * 2 + 6` = 周期数）；
- 关键怀疑点：写 MR0 之后**控制器的 read-latency/dummy 周期必须跟着改**，
  我这边只改了片子、没改控制器。

**另外还有一批"bootloader 专属"的前置步骤必须自己补上** —— 因为我的 bootloader
把 IDF 的二级 bootloader 顶掉了，而这些东西**原本在那边**做
（`bootloader_support/src/esp32s31/bootloader_esp32s31.c:52-67`）：

| # | 写什么 | 地址 | 值 |
|---|---|---|---|
| 1 | `modem_lpcon` 总线时钟 | `0x20587040` | bit0 = 1 |
| 2 | 模拟 I²C 主机时钟 | `0x2010f018` | bit2 = 1（已做） |
| 3 | 强制使能 | `0x2010f01c` | bit2 = 1 |
| 4 | 时钟源选 160M | `0x20109c04` | bit12 = 1 |
| 5 | PSRAM 时钟源先切回 XTAL | `0x20587068` | bits[6:5] = 0 |
| 6 | `I2C_BIAS` `DREG_1P1` / `DREG_1P1_PVT` = 10 | `0x2010f800`(I2C_ANA_MST) | 10 |

第 6 条（模拟 bias）最可疑：它不在，MPLL 的模拟部分就没有正确偏置，
既可能让自校准不收敛，也**可能解释为什么 mode register 读出来是歪的**
（vendor 读成 `0x8d` 而 IDF 是 `0x0d`、density 读成 `0x06` 而 IDF 是 `0x05`）。

> 已核实：S31 的二级 bootloader **从不打开 MPLL**（`bootloader_esp32s31.c:65` 反而是
> `clk_ll_mpll_disable()`），MPLL 的开启+标定整个在 **app 的**
> `call_start_cpu0 → mspi_init → esp_psram_impl_enable` 里
> （`esp_system/port/cpu_start.c` 根本不编进 bootloader）。所以"照 bootloader 抄"是抄不到的，
> 必须照 app 的 `esp_psram_impl_ap_oct.c` 抄。

### 第 5 轮补充：两个"看起来很吓人其实正常"的现象 + 一个必须保持关闭的开关

**① 🚨 MPLL 校准路径必须保持关闭（`PSRAM_ALLOW_MPLL_CAL 0`）**

冷启动（真正断电后再上电）时 ROM 留下的 `MSPI_CAL_END = 0`，会走进
`清 CAL_STOP → 死等 CAL_END` 那段。而 MPLL 的模拟偏置前置还没补齐
（缺 bootloader 专属的 `I2C_BIAS DREG_1P1` 等），校准**根本收敛不了** →
**CPU 卡在那段轮询里，连 USB 都枚举不出来**，现象就是"接 GND 复位后看不到盘符"。
热复位时因为 IDF 之前留下的 `CAL_END=1` 会跳过校准，所以**只有冷启动才犯**——
这种"只在某些复位类型下才复现"的 bug 最难查，别再打开它，直到 I2C_BIAS 补齐并验证过。

**② 复位后盘符会消失几秒 —— 这是正常的**

每次复位 USB 设备都重新枚举，Windows 要过几秒才重新挂上盘符。**别急着下结论**，
先分清"设备在不在"：

```powershell
Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'PID_101A'   # USB-MSC 设备在不在
Get-Volume | Where-Object FileSystemLabel -eq 'S31-BOOT'                 # 盘符挂没挂
```
- **设备不在** → 固件没跑到 USB（看 COM43 日志停在哪一行）
- **设备在、卷不在** → Windows 没挂盘，等一等 / 重新枚举 / 手动分配盘符

**③ 复位后盘里的文件"消失"了 —— 也是正常的（不是丢数据）**

这个盘是**虚拟 FAT16：FAT 元数据（引导扇区/FAT/根目录）只存在 RAM 里**，
只有**数据区**是 1:1 落到 flash 的。所以复位后根目录被重新生成、看起来是空盘。
**镜像本身安全地躺在 flash 数据区**，而且描述符里记着 `src_off / size / crc`，
bootloader 只认描述符，不认盘上有没有文件。
（真正的落盘证据在 finalize 那几行：`flush done: flash(erase=N prog=N)`，
N 必须 > 0。⚠️ 上方的状态行 `flash(erase=0 prog=0)` 是**每 5 秒**打的，
finalize 之后紧接着就复位，所以从状态行看永远是 0 —— 本项目作者就被它骗过一轮。）

**④ 别把垃圾写进 PSRAM 的 mode register**

顺序必须是"**先做基础读写自检，通过了才写 mode register**"。
理由是 mode register 要**读-改-写**，而我们的寄存器读相位是歪的
（vendor 读成 `0x8d` 而 IDF 是 `0x0d`、density 读成 `0x06` 而 IDF 是 `0x05`）→
拿这种读值回写 = 把错误的 latency / drive strength 烧进芯片，
而它是**断电才复位**的。实测两次写进去的值还不一样（`0x8d4a` / `0x8d42`），
等于在看不见的地方持续伤害硬件。现在 `psram_write_mode_reg()` 是**严格**判 vendor
（不再 `& 0x7F` 绕过），基础读写不通就一行都不写。

### 第 5 轮补充 2：串口助手显示中文乱码 —— 别去改固件

现象：App 跑起来后**某一段中文是乱码**，而同一行里的 ASCII（`0x2f0608e8`）完好。
**固件没问题**：源码是合法 UTF-8、字节正确发到线上，是**串口助手按 GBK 在解 UTF-8**。

一眼判定的办法（不依赖终端编码，只看**字数**）：

| 源码片段 | UTF-8 字节数 | 当 GBK 显示的字数 |
|---|---|---|
| `本函数地址` | 15 | **8** |
| `（应在 ` | 10 | **6** |
| `期望` | 6 | **3** |

对得上 → 就是终端编码问题。**ASCII 会完好无损**（ASCII 自同步），
只有中文段会变成等量的别的汉字 + `�`，看着就像"只有某一段坏了"。
（本工作区 `AGENTS.md` §4.6 记过同一条：GBK 控制台里中文日志必然乱码。）

**怎么办**：
1. 把串口助手的显示编码改成 **UTF-8**（SSCOM 有些版本没有这个选项，那就用第 2 条）；
2. 用本工作区自带的读取器，它强制 UTF-8：`python tools\read_port.py COM43 10`；
3. 真的只能用 GBK 助手看，就把日志改成 ASCII —— 但工作区的约定是日志用 UTF-8，不推荐。

### 第 5 轮补充 3：🚨 主机把"枚举失败"缓存住了 —— 设备管理器里「未知 USB 设备(设备描述符无效)」

**现场特征（三条同时出现就是它）**：
1. 设备管理器里出现 `未知 USB 设备(设备描述符无效)`，InstanceId 是 **`VID_0000&PID_0005`**
   （0000/0005 是 Windows 描述符读取失败时给的占位 ID），且挂在与 USB-JTAG **不同的** root hub 口上；
2. 固件侧日志一切正常：`[usb:before]/[usb:after]` 的寄存器自检**与正常时逐位相同**、
   `[usb] ready` 照常打出；
3. 但 **`cfg=0` 永远不变、`rd/wr` 全 0** —— 主机压根没来配置。

⇒ **不是固件坏了，是主机侧把那个端口的失败状态缓存住、不再重试**（拔插过快、
上一次复位时 PHY 还没稳就挂上总线等都会触发）。Windows 不会自己恢复。

**处置（从便宜到彻底）**：
1. **把 USB-HS 线拔下来等 3 秒再插上** —— 九成情况这一下就好；
2. 设备管理器里右键那个未知设备 → **卸载设备** → 扫描硬件改动；
3. **换一个 USB 口**（优先主板后置口，别用前面板/hub）；
4. **整板断电**（两条 USB 线都拔掉等 5 秒）—— 顺带清掉 PHY/DWC2 的残留状态。

**已内置的自救**：`s31_usb_soft_reconnect()`（`bsp/s31_usb_glue.c`）—— 写 DWC2 `DCTL.SDIS`
撤掉 D+ 上拉再恢复，**对主机完全等价于"拔了再插"**。主循环里只要 `cfg=0` 超过
`RECONNECT_EVERY_MS` 就自动来一次，所以**这个故障现在不用人插手，固件自己会救回来**。

⚠️ **这个阈值必须明显大于"主机正常枚举所需时间"**：实测本机 Windows 从设备挂上到
`cfg=1` 最慢要 **~5 秒**。我第一版设了 4 秒 —— 那会在正常枚举到一半时把设备拔掉，
**自己把自己打断**（改 10 秒后复测：`cfg=1` 在 t=5034 ms，软拔插 0 次触发）。
→ 通则：**任何"超时重试"的时间常数，都要拿"正常情况的最慢观测值"当基准，不能拍脑袋。**

**⑤ 软拔插的"断开时长"才是成败关键（第 2 版才修对）**

第一版 `s31_usb_soft_reconnect()` 只断开 **60 ms** —— **实测完全无效**：
固件日志里 `[usb] 主机一直没配置（cfg=0）-> 软拔插重试` 每 10 秒老老实实打一条，
而主机侧那个 `VID_0000&PID_0005` 纹丝不动。
对照实验给出了答案：**能救回来的那次是"烧录"** —— esptool 进下载模式时
USB 设备会**真的消失好几秒**，主机这才把"描述符读取失败"的缓存清掉并重新枚举。
→ 所以断开时长改成 **1500 ms**（`S31_USB_RECONNECT_OFF_MS`）。
**60 ms 对 Windows 来说太短，它根本来不及处理 detach 事件。**

**⑥ 状态行现在带 USB 诊断位（排查这类问题一次到位）**

```
[t=  5034 ms] cfg=1 rd=23 wr=18 ... evt=05 irq=995 gint=4480842c dsts=0014fd00   ← 正常
```

| 字段 | 含义 | 判读 |
|---|---|---|
| `evt` | CherryUSB 事件位（1=RESET 2=CONNECTED 4=CONFIGURED） | `05` = 连上且配置好 |
| `irq` | USB ISR 进过几次 | **0 = 主机压根没跟我们说话**（主机侧/电气问题，不是固件） |
| `gint` | DWC2 `GINTSTS` | 有 bit 但 `irq` 不涨 → 中断没送到 CPU = CLIC 路由问题 |
| `dsts` | DWC2 `DSTS` | **bit[2:1] = 速度：0=High Speed，1=Full Speed** |

- 正常基线：`evt=05 irq≈1000 cfg=1 dsts=0014fd00`（高速）；
  `usb_hw_init` 刚做完时 `dsts=...02`（= Full Speed，还没 chirp）属正常。
- 之前那种"无法识别"的情形是 **`cfg=0` 且 `irq` 不涨** → 主机没发东西，固件无责。

### 第 5 轮补充 4：🚨 库日志会在 **USB 中断里**打出来 —— 必须加中断闸门

**这条是"问对了问题"问出来的**（用户直接问"你在 usb 中断中有没有打印？"）。

- **我们自己的代码没有**：`s31_usb_isr()` 只计数，`usbd_event_handler()` 只置标志位。
- **但 CherryUSB 库有，而且就在中断路径里**：`bsp/usb_config.h` 里
  `CONFIG_USB_DBG_LEVEL = USB_DBG_INFO`，而 `USB_LOG_ERR` 在**任何**等级下都开着
  （`USB_DBG_ERROR = 0` 是最低等级，`>=` 判定必然成立）。全库 ~20 个活跃调用点，中断里的有：

  ```c
  // usbd_core.c —— EP0 控制请求分发器（跑在 USB 中断里）
  L911:  USB_LOG_ERR("standard request error\r\n");  usbd_print_setup(setup);
  L918:  USB_LOG_ERR("class request error\r\n");     usbd_print_setup(setup);
  L925:  USB_LOG_ERR("vendor request error\r\n");    usbd_print_setup(setup);
  ```
  而 `usbd_print_setup()`（usbd_core.c:88）会把**整个 SETUP 包 dump 出来**（约 70 字节）。

- **两层害处**：
  1. **正反馈**：中断里打印 → ISR 变慢 → 主机控制传输超时（表现就是
     「设备描述符无效」）→ 报更多错 → 打印更多。与 `AGENTS.md` §4.7 那个坑同类。
  2. **可重入性**：`kprintf` 的发送环形缓冲**不是可重入的**。主循环每 5 秒打一次状态行，
     若那一刻 USB 中断插进来也打日志，`head` 指针更新会丢 → 字节被覆盖 / 环形缓冲卡死。
     **这也是日志出现乱码的来源之一**（另一来源是终端 GBK 解码，见补充 2）。

- **修法**：不关日志（初始化那批 `[I/USB]` 参数 dump 很有用），而是**加闸门** ——
  `bsp/s31_usb_glue.c` 用 `g_usb_in_isr` 标记"正在 USB ISR 里"，
  `usb_config.h` 把 `CONFIG_USB_PRINTF` 改成"中断上下文直接丢弃"：

  ```c
  #define CONFIG_USB_PRINTF(...)  do { if (!g_usb_in_isr) { kprintf(__VA_ARGS__); } } while (0)
  ```

- **验证**：`[I/USB] GSNPSID:...` 等初始化日志照常输出（非中断路径），
  同时 `cfg=1 evt=05 irq=1001` 枚举正常、日志里 `[E/USB]`/`Setup:` 一条都没有。

> 通则：**给库的日志后端一律加"中断闸门"**，除非你确认过它绝不会从 ISR 里调用。
> 这条对任何第三方协议栈都成立，不只是 CherryUSB。

### 第 5 轮结论：USB 稳定性问题解决了 —— 4 个真 bug，按贡献排序

用户最终反馈"现在非常稳定了"。回头看，**不是一个 bug，是四个叠在一起**，
而且每一个单独看都能自圆其说，所以才会来回好几轮。按实际贡献排序：

| # | Bug | 症状 | 修法 |
|---|---|---|---|
| 1 | **CPU 只有 40MHz**（bootROM 交棒值，从没提过频）| 我们的 EP0+MSC **全在中断里做**，比 IDF 的 320MHz 慢 8 倍 → 时好时坏 | `bsp/s31_cpuclk.c`：CPLL `40×8/1 = 320MHz`。**移植自 `projects/rtt_nano_s31/bsp/drv_clk.c`**（同板裸机、已实测过）|
| 2 | **跳 App 前没关 USB PHY** | PHY 上电位在常电域（HP_ALIVE/LP_SYS）**跨复位保留**，而跳 App 那条路径**根本不调 `usbd_initialize()`** → 主机看到不回答的僵尸设备 → 缓存「描述符无效」→ 之后怎么复位都不认 | `s31_usb_phy_off()`，在 `app_probe()` 跳转前调用 |
| 3 | **库日志在 USB 中断里打印** | `USB_LOG_ERR` 恒开，`usbd_core.c:911/918/925` 在 EP0 中断路径 dump 整个 SETUP 包 → ISR 变慢 + 环形缓冲不可重入（乱码来源之一）| `g_usb_in_isr` 闸门，中断上下文丢弃 |
| 4 | **软拔插断开太短** | 60ms 对 Windows 太短，它来不及处理 detach → 救不回来（对照：**烧录**能救，因为下载模式真的断好几秒）| 断开时长改 **1500ms** |

**方法论教训（比这 4 个 bug 值钱）**：

1. **变量隔离靠"已知 good 基线"**：用户提议拿 `cherryusb_cdc` 改一个官方 MSC 出来对照 ——
   结果它 **4/4 稳定**、而本固件时好时坏，**一句话就把"是不是硬件/线/主机的锅"排除了**，
   逼着往自己代码里找。工程产物留在 `projects/cherryusb_msc`（`make run PROJ=cherryusb_msc`）。
2. **对照实验要控制"复位方式"**：我全程用 esptool 复位（`rst:0x17`，设备会真断开几秒），
   所以**怎么试都是好的**；用户按物理复位/跑过 App 才复现 —— 这也是 bug 2 藏这么久的原因。
   → 复现不了时先问一句：**"你的操作路径和我的差在哪？"**
3. **别信二手结论**：`SystemCoreClock = 40000000u` 是抄来的注释、从没验证过；
   写个 `mcycle × SYSTIMER` 一量就有数（还顺带发现参考工程的硬件回读寄存器在 S31 上不可信）。
4. **参考工程就在手边**：`rtt_nano_s31` 早就有能跑的提频代码，我先绕去 IDF 挖了半天 ——
   用户一句"你都忘记了？"点醒。**同板同裸机的参考 > 上游抽象层**。

**当前状态**：USB 侧 ✅ 稳定（320MHz 下连续 4/4 复位全部枚举成功）；
**唯一剩下的**是需求里的"App 的 VMA 放 PSRAM"（M3）—— 后来在第 6~9 轮解决了，
结论见 [PSRAM.md](PSRAM.md)。

---

### 第 6 轮：⭐ PSRAM 通了 —— 两个真凶，一个解决、一个如实记录

> **完整版（含全部证据、排除清单、方法论、复现命令）在 [PSRAM.md](PSRAM.md)。**
> 这里只留最短的摘要。

#### 真凶一（已解）：mode register 三个位域全写错了

IDF 的 `oct_psram_mode_reg_t` 位域才是唯一权威，我之前是凭印象拍的：

| 寄存器 | ✅ 正确 | ❌ 我写的 | 我写成了什么意思 |
|---|---|---|---|
| MR0 | `(4<<2)\|(1<<5)` = **0x30** | `(4<<4)\|(1<<1)` = 0x42 | 读延迟 **0**、lt=**可变延迟** |
| MR4 | `1<<5` = **0x20** | `1<<4` = 0x10 | 写延迟 **0** |
| MR8 | `3\|(1<<3)` = **0x0B** | `(3<<5)\|(1<<2)` = 0x64 | **x16 数据模式** + hybrid burst |

**为什么难认**：mode register 用固定 dummy 访问，所以**寄存器读写全对、回读逐位一致**，
**只有阵列读写是垃圾** —— 所有线索都指向"控制器采样点/dummy/tuning"，方向全错。

**顺带澄清两个我自己造的坑**：
- 读 MR0 得 `0x8d42` **不是"相位偏一位"** —— `mr1 = 0x8D`，而 vendor_id 只占**低 5 位**：
  `0x8D & 0x1F = 0x0D = AP` ✔（"看到 0x8d 就拒绝写"这个判断才是那个死循环的根源）
- 顺序必须是 IDF 的 **先写 mode register、再检查连接性**；反过来（先检查、通过了才写）
  在冷启动时必然 MISMATCH → 永远走不到写入 → 永远 MISMATCH

结果：`参考字: 写 5a6b7c8d 读 5a6b7c8d -> OK`，IDF 原版实现也返回 OK 且日志逐字段一致。

#### 真凶二（未解，已记录）：**cache 写外部存储 = store access fault**

- **读完全正常，而且是真 L1 命中**：`mcycle` 实测反复读同一字 —— 内部 RAM 5 拍/次、
  flash 窗口 **4** 拍/次、PSRAM 窗口 **4** 拍/次（不可能是每次下 MSPI）
- **写一律异常**：`0x40000000` / `0x50000000` / `0x70000000` 全一样，`mcause=0x30000007`
- **已逐项排除**：MMU 页表项（与 IDF 逐位一致、VALID）、PMP（全 0）、PMS 权限段（全允许）、
  MSPI 错误位（无）、cache 时钟 `force_on`（已补且与 IDF 一致）、cache 未复位、
  CPU/MEM/SYS/APB 分频（与 IDF 一致）、**342 个寄存器与 IDF 真值机机械对差（无实质差异）**
- ⚠️ **别拿 flash 窗口当"可写对照组"**：mmap 出来的 flash 地址本来就是只读的，
  IDF 也一样 `Store access fault`（工作区 AGENTS.md §4.16 有记录）

#### 交付的写路径：原始 MSPI 事务（**IDF 自己也这么干**）

`psram_ctrlr_ll_common_transaction` + 64 字节分块 —— 与 IDF 往 PSRAM 写参考数据的
`mspi_timing_by_dqs.c` 是**同一招**。已接进 `app_image.c`（PSRAM 段 + bss 都走它，
写完 `Cache_Invalidate_Addr`）。

```
[psramLL] ★ 原始写 + 原始读回: OK (0/256 错)
[psramLL] ★ 原始写 + cache 读回: OK (0/256 错, 异常 0 次)
[psram]   4KB pattern test: PASS (0 errors)
```

**⇒ 由此得出 App 的设计约束：`.text`/`.rodata` 放 PSRAM（只读，走 cache 读），
`.data`/`.bss` 必须放内部 RAM（运行时会被写）。**

#### 方法论（比结论值钱）

1. 🥇 **拿 IDF 当"真值机" + 机械对差**：新建 `projects/psram_probe`（IDF 工程，
   sdkconfig 从 `s31_membench` 拷），把自己能正常读写 PSRAM 的那一侧寄存器按
   `R <addr> <value>` 整块打印；裸机侧同格式打印后做集合 diff。
   **这是本轮唯一收敛的手段**（盲猜寄存器猜了十几轮都不收敛）。
   ⚠️ 边界：**写-only / 只锁存一次的寄存器 diff 看不到**，所以"无差异"≠"状态一致"。
2. **测量要能证伪**：`mcycle` 量"反复读同一字"是唯一能证明"cache 真的在工作"的判据
   （IDF 自己注释：*There's no register indicating cache enable/disable state*）。
3. **对照组的选取决定结论对错**：第一版探测对所有地址都写，于是"flash 窗口也异常"
   把结论带偏了——而 flash 本来就不可写。
4. 🚑 **停机循环里狂灌 USB FIFO 会把板子搞到"设备不识别"**，
   靠 `openocd -f board/esp32s31-builtin.cfg -c "init" -c "reset run"` 救回来（不用断电）。
   详见 [PSRAM.md](PSRAM.md) §8。

#### 本轮新增/改动

`bsp/s31_psram_ll.c`（位域修正 + `s31_psram_write_raw/read_raw` + `s31_cache_clk_init` + 一堆待收敛的诊断）、
`app/app_image.c`（PSRAM 段走 raw 写）、`bsp/s31_psram.c`（`crc_test` 改成 raw 写 + cache 读）、
`bsp/trap_handler.c`（守卫支持非法指令 + 停机循环加延时）、
`bsp/linker.ld`（`PROVIDE(EFUSE/CACHE)`）、`tools/build.ps1`（3 个 IDF include 路径）、
**新增 `projects/psram_probe`（IDF 真值机）**。

---

### 第 7 轮：⭐ **App 真的从 PSRAM 跑起来了**（+ 又排除掉一批）

> 完整版见 [PSRAM.md](PSRAM.md) §10。

**交付**（板上实测，拖入 4486B 的 `app.bin` → 自动复位跳转）：

```
[app] .text   50000000..50000ad8  (2776 B)   ← PSRAM
[app] .rodata 50000ad8..50000f7a  (1186 B)   ← PSRAM
[app] .bss    2f060150..2f061360  (4624 B)   ← 内部 RAM
[app] &app_main = 500007ac     &s_banner = 50000f5c     sp = 2f079fd0
[app] 结论: OK —— 代码在 PSRAM、可写数据在内部 RAM（就是从 PSRAM 取指跑起来的）
[app] alive #1 ... #22   t=21ms..21046ms
```

⇒ **"LMA 在 flash、VMA 在 PSRAM" 达成**；**但从 PSRAM 取指**这一条也顺带验证了
（I-cache + IBUS1 + PSRAM MMU 这条路是通的）。

⚠️ **"全部放 PSRAM、不用 SRAM" 做不到** —— 卡在同一处：App 一跑就要写 `.data`/`.bss`/栈，
而**运行时写 PSRAM 会 store access fault**。

**第 7 轮又排除掉的**（详见 PSRAM.md §10.3）：

| 候选 | 为什么不是它 |
|---|---|
| **CPU_APM 区域过滤器** | 它是**真开着**的：region0 覆盖 `0x40000000..0x5fffffff`、`ATTR=0x7000` = 只有 TEE_MODE 有 R/W/X，三种 REE_MODE 全禁。但**把它整个关掉（FILTER_EN=0 + attr=0x7777）写照样异常** |
| HP_APM / HP_MEM_APM / TEE | 都 dump 了，region 要么全 `ffffffff`、要么是内部 RAM，都盖不到 PSRAM 窗口 |
| cache 自己的"访问失败"判定 | `CACHE_L1_CACHE_ACS_FAIL_CTRL=0x13`（check mode=1，失败会传播给请求）很像凶手，但清了记录、开了 INT_ENA、再做一次 store —— **`D_ADDR/ID_ATTR/RAW/ST` 全是 0，cache 压根没记这笔** |
| `SPI_MEM_S_CTRL1`（AR/AW SIZE 支持位） | `0x2ec00000` 与 IDF 逐位一致 |
| 核心 1 干扰 | `HPCORE1_CTRL0=4` → core1 **一直被摁在复位里**，不可能干扰 |
| `0x50000000` 这个地址有问题 | IDF **在 0x50000000 上写得进去**（写回+失效后 0 错） |

**⚠️ 一个差点把自己带偏的测试陷阱（方法论，值得单独记）**：

我一度怀疑"IDF 能写 PSRAM 也是假象"（store 被静默丢弃、读命中 cache → 自检假 PASS）。
第一版判据用 `Cache_Invalidate_Addr()` → **16384/16384 全错**，看着像坐实了。

**但这个判据是错的**：`Cache_Invalidate_Addr` **丢弃脏行、不回写** —— 等于亲手把数据扔了再去找。
换成 **`Cache_WriteBack_Invalidate_Addr()`** 之后，IDF 在 0x50000000 / 0x50010000 / 0x50100000 /
0x50400000 **全部写得进去**。⇒ **bug 是我们的，不是硅片、也不是地址。**

> 配套注意：测的数据量必须**远大于 cache**，否则整块都在 cache 里、一行都不会被挤出去
> （这次 `CACHE_L1_DCACHE_CACHESIZE=64K`，我刚好写了 64KB，所以是"全错"而不是"最后一段错"）。

**下一步最省时间的两条 A/B 实验**（PSRAM.md §10.5）：
1. 在 IDF 的 `psram_probe` 里**先做一次 MSPI3 原始事务写**，再试 cache 写 —— 若 IDF 也因此写不进去，就锁定"**原始事务把 MSPI2(AXI/cache 侧) 带歪了**"，而 IDF 的 `mspi_timing_psram_tuning()` 最后一步正好会恢复它（我们至今仍是 stub）。
2. 反过来：裸机里**跳过所有 PSRAM 数组原始写**（连参考字检查都不做），直接映射 MMU + cache 写，看能不能写。

**本轮顺手修的两个工程问题**：
- `mkapp.py` 改成**真·按段打包**（`objcopy -O binary` 会把 `0x2F060000` 和 `0x50000000` 之间
  的 ~550MB 空洞拍进镜像）；
- `s31_psram_invalidate_cache()` 改成 **I/D 都失效**（App 的 `.text` 也在 PSRAM，
  只失效 D-cache 可能执行到陈旧指令）。

---

### 第 8 轮：把 PSRAM 整理成**一个文件**（代码瘦身 19.9MB → 0.2MB）

> 详见 [PSRAM.md](PSRAM.md) §11。

```
bsp/:  19.9 MB / 535 个文件  ->  0.2 MB / 16 个文件
bootloader app.bin:  47.5 KB ->  39.2 KB
```

**PSRAM 现在只有 `bsp/s31_psram.c`（609 行）**，对外 5 个函数：
`s31_psram_init` / `write_raw` / `read_raw` / `invalidate_cache` / `crc_test`。

删掉的：所有诊断函数、A/B/C/D/E 那一串试验、IDF 原版实现那条对照路径
（`s31_psram_idf_init` + `idf_psram_impl.c` + `idf_psram_shim.c`）、
早期手写版的死代码、`tools/gen_sdkconfig_h.py`、以及
**`bsp/idf_port/`（19.9MB 的 IDF 头文件倾倒场）**。

**「太复杂的宏展开」是怎么消掉的**：`psram_ctrlr_ll_xxx()` 是
`do { (void)__DECLARE_RCC_ATOMIC_ENV; _psram_ctrlr_ll_xxx(...); } while(0)` 套壳宏。
**但只有 5 个函数有这种壳**，直接调**下划线版本**（普通 `static inline`）即可 ——
**IDF 自己的二级 bootloader 就是这么调的**。现在文件里没有一处
`__DECLARE_RCC_ATOMIC_ENV`、没有一处 `(void)0`。

**但 LL 头文件本身留着**（`hal/psram_ctrlr_ll.h` / `mspi_ll.h` / `mmu_ll.h`）——
本项目因为手抄寄存器位号栽过两次（`FB_DIV` 的 bit[7:3]、
AP PSRAM 的 MR0/MR4/MR8），所以位号交给 IDF 的头文件，我们只写字段名。

整理后**完整回归验证通过**：App（`.text` 在 PSRAM）照常从 PSRAM 取指运行。

**整理后的全部日志就 6 行**：
```
[psram] === 初始化（16MB 8 线 DDR @200MHz）===
[psram] 芯片 = AP 系列 (vendor 0x0d)
[psram] mode reg: MR0=30 MR4=20 MR8=0b（期望 30/20/0b）
[psram] 参考字: 写 5a6b7c8d 读 5a6b7c8d -> OK
[psram] MMU: 256 页 @50000000 -> PSRAM 物理 0（首项 00000c00）
[psram] 自检: 原始写+原始读 OK / 原始写+cache读 OK -> PSRAM 可用
[psram] pattern test: PASS (0 errors)
```

---

### 第 9 轮：★★★ **真凶是 PMA** —— PSRAM 写彻底解决，App 全放 PSRAM

> 完整版见 [PSRAM.md](PSRAM.md) §12。

**答案**：这颗 CPU 有 **PMA（Physical Memory Attribute）**，IDF 把它实现成
**自定义 CSR**（`CSR_PMACFG(i) = 0xBC0+i` / `CSR_PMAADDR(i) = 0xBD0+i`）。
本板 ROM 留下的现场：

| 条目 | cfg | 区间 | 权限 |
|---|---|---|---|
| PMA14 | `C000001D` | `[0x2F000000,+512KB)` IRAM | R+W+X |
| **PMA15** | **`C0000015`** | **`[0x40000000,+512MB)` 外部存储** | **R+X ← 没有 W** |

`[0x40000000,+512MB)` 正是 cache 的外部存储虚拟窗口，**flash 映射窗口和 PSRAM 窗口
都在里面**。ROM 把整段标成只读 → 读全对、能执行、**任何 store 都 fault**，
而且 flash 窗口和 PSRAM 窗口表现一模一样（对 PMA 来说它们是同一段）。

**IDF 那边的权威写法**（`port/esp32s31/cpu_region_protect.c`，注释原文
*"without setting this, psram cannot be reached"*）：
```c
PMA_RESET_AND_ENTRY_SET_NAPOT(7, SOC_EXTRAM_LOW, SOC_EXTRAM_HIGH-SOC_EXTRAM_LOW,
                              PMA_NAPOT | PMA_RWX);
```
它由**二级 bootloader** 调用（`bootloader_mem.c:63`）——**我们的裸机 bootloader
顶掉了二级 bootloader，所以这一步从来没人做。**

**修法**（`bsp/s31_psram.c` 的 `pma_grant_write()`，比 IDF 保守：不重置整张表）：
扫 16 条 PMA，把覆盖 PSRAM 窗口的那条补上 `PMA_W`：
```
[psram] PMA15 [40000000,+524288KB) 补上写权限: c0000015 -> c000001d
```

**为什么查了这么久（三条教训）**：
1. 🚨 **PMA 是 CSR，不在内存映射空间里 —— 任何"寄存器对差"都看不见它。**
   我们拿 IDF 当真值机逐块比过 **342 个寄存器**全都对得上；
   **"全都对得上"本身就是最强的线索**：差异一定在非内存映射状态里。
2. 🚨 "能读不能写"要第一时间想到 **PMA/PMP 这类权限表**，而不是 cache 策略。
3. ✅ 两个判别实验直接砍到"CPU 核内"：
   **旁路 L1 D-cache 后写仍异常**（→ 不是缓存策略）**而读照常成功**（→ 是权限）。

**结果 —— App 全部放 PSRAM，内部 RAM 一点不用**（`projects/s31_app/bsp/linker.ld`
改成单区域全 PSRAM，连 `.boot`/`.clic_entry`/栈都在里面）：

```
[app] .text   5000013c..50000c14  (2776 B)
[app] .rodata 50000c18..50001096  (1150 B)
[app] .bss    500010a0..500022b0  (4624 B)
[app] &app_main = 500008e8   &s_magic = 50001098
[app] sp = 50ffffd0                                   ← 栈在 PSRAM 末尾
[app] .data magic = 0x1234abcd    .bss counter = 0
[app] 结论: OK —— 代码/常量/可写数据/栈全部在 PSRAM，一点内部 RAM 都没用
[app] alive #1 ... #4
```

