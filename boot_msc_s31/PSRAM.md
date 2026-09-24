# PSRAM 攻坚战 —— 从"参考字 MISMATCH"到"能装载 App"

> 本文只讲一件事：**让 ESP32-S31 的 16MB 片内 PSRAM 在裸机 bootloader 里真正可用**。
> 这是本项目耗时最长的一块（前后 6 轮），过程里排除了十几个错误方向，
> 所以把**结论、证据、方法论、以及"别再往哪儿填坑"**都记全。
>
> - 想要**结论** → 看 §0 TL;DR + §2（真凶一）+ §12（真凶二：PMA）
> - 想要**过程** → [DEBUG-LOG.md](DEBUG-LOG.md)（按轮次的流水账，与本文 §10~12 有重叠，以本文为准）
> - 想要**设计** → [README.md](README.md) §3（App 的内存模型）
>
> 📌 本文 §3、§6、§10.2、§10.5 写于"还没找到 PMA"的时候，
> **里面的"未解 / 做不到 / 下一步查这几个"都已经被 §12 推翻了** ——
> 保留了原文（它们记录了当时的推理），但每节开头都加了指向 §12 的批注。

---

## 0. TL;DR

| 问题 | 结论 |
|---|---|
| PSRAM 芯片初始化 | ✅ **通了**。IDF 原版 `esp_psram_impl_enable()` 返回 OK，日志与 IDF 参考**逐字段一致** |
| 曾经的"参考字 MISMATCH" | 🎯 **真凶是 mode register 三个位域全写错了**（我凭印象拍的，不是 IDF 结构体的布局） |
| MMU 映射 | ✅ 页表项格式与 IDF 逐位一致，且被一份**独立的裸机 P4 参考**佐证 |
| PSRAM **读**（走 cache） | ✅ 正常，且**确实是 L1 命中**（mcycle 实测 4 周期/次） |
| PSRAM **写**（走 cache） | ✅ **已解决** —— 真凶是 **PMA**：ROM 把外部存储窗口 `[0x40000000,+512MB)` 标成只读，二级 bootloader 本该用 `esp_cpu_configure_region_protection()` 打开它。见 **§12** |
| PSRAM **写**（走原始 MSPI 事务） | ✅ 也通；**装 App 仍然用它**（原因见下面"一句话"） |
| App 放 PSRAM | ✅ 代码/常量/可写数据/栈**全都能放 PSRAM**（§12.4 实测）。当前 App 模板用的是"LMA 在 flash、VMA 在 PSRAM"的经典模型，见 [README.md](README.md) §3.0 |

**一句话**：PSRAM 现在是**可读、可写、可执行、可装载**的。
"写"走 cache 已经没问题了（PMA 修好之后）；**装 App 之所以还走原始 MSPI 事务，
是因为那条路绕过 cache、不会留下陈旧行要清**，跟"能不能写"无关。

---

## 1. 起点与终点

**起点**（第 5 轮结束时）：

```
[psramLL] mode reg 已写 MR0=0042 MR4=0010 MR8=64（回读 8d42/6410/64）
[psramLL] 写完 mode reg 后参考字回读: MISMATCH
[psramLL] PSRAM 不可用 -> 放弃
```

看起来"寄存器写进去了、回读也对得上，但阵列就是读不出来"。当时的怀疑方向是
**DDR 读采样点/dummy 长度/timing tuning**——全都错了。

**终点**（现在，板上实测）：

```
[psramLL] 写前 MR0=72 MR1=8d(vendor=0d=AP) MR4=30 MR8=6b
[psramLL] 写后 MR0=72(期望72) MR4=30(期望30) MR8=6b(期望6b)
[psramLL] 参考字: 写 5a6b7c8d 读 5a6b7c8d -> OK
[psramLL] ★ 原始写 + 原始读回: OK (0/256 错)
[psramLL] ★ 原始写 + cache 读回: OK (0/256 错, 异常 0 次)
[psram]   4KB pattern test: PASS (0 errors)
```

---

## 2. 真凶一：mode register 的位域（这一步解开了一切）

### 2.1 位域到底长什么样

**唯一权威**是 IDF 自己的结构体 `components/esp_psram/device/esp_psram_impl_ap_oct.c`
里的 `oct_psram_mode_reg_t`（C 位域自低位排起）：

```
mr0:  drive_str[1:0]   read_latency[4:2]   lt[5]   rsvd6[6]   tso[7]
mr1:  vendor_id[4:0]   rsvd5_7[7:5]        ulp[8]              ← vendor_id 只占低 5 位！
mr2:  density[2:0]     dev_id[4:3]         kgd[7:5]
mr4:  pasr[2:0]        rf[4:3]             wr_latency[7:5]
mr8:  bl[1:0]          bt[2]   rbx[3]      rsvd5[5:4]  x16[6]  rsvd7[7]
```

200MHz 档 IDF 要的值（`mode_reg.mr0.lt=1; mr0.read_latency=AP_OCT_PSRAM_RD_LATENCY(4);
mr0.drive_str=0; mr4.wr_latency=1; mr8.bl=3; mr8.bt=0; mr8.rbx=1; mr8.x16=0`）就是：

| 寄存器 | ✅ 正确值 | ❌ 我原来写的 | 我写的意思变成了 |
|---|---|---|---|
| **MR0** | `(4<<2) \| (1<<5)` = **0x30** | `(4<<4) \| (1<<1)` = 0x42 | read_latency=**0**、lt=**0（可变延迟）**、rsvd6=1 |
| **MR4** | `1<<5` = **0x20** | `1<<4` = 0x10 | wr_latency=**0** |
| **MR8** | `3 \| (1<<3)` = **0x0B** | `(3<<5) \| (1<<2)` = 0x64 | bl=0、bt=1、**x16=1（16 位数据模式！）**、rbx=0 |

### 2.2 为什么这个 bug 这么难认

**症状极具欺骗性**：mode register 用**固定 dummy 长度**访问，所以

- ✅ 寄存器写得进、读得出、**回读逐位一致**
- ✅ 控制器那边（DDR/8 线/AXI/splice）全部配好，没有任何报错
- ❌ **只有"阵列读写"是垃圾**——因为芯片被告知"读延迟可变、写延迟 0、16 位数据模式"

于是所有线索都指向"控制器的采样点/dummy/tuning"，而真正错的是**往芯片里灌的配置值**。

### 2.3 顺带澄清一个我自己造的误判

写前回读 MR0 得到 `0x8d42`，IDF 期望 vendor id 是 `0x0D`，我当时判断成
"**我们的读相位偏了一位**"，于是改成"用常数构造、不做读-改-写"，还加了
"读到 0x8d 就拒绝写"的逻辑。

**全错。** 16 位读一次填两个字节（小端）：

```
0x8d42  ->  mr0 = 0x42（低字节）   mr1 = 0x8D（高字节）
vendor_id 是 mr1 的低 5 位：0x8D & 0x1F = 0x0D = AP  ✔ 完全正确
```

**教训**：跨字节/跨寄存器的"读出来不对"要先确认**字段宽度**，再怀疑硬件。
`0x8D` 不是错值，它只是"vendor_id + 3 个保留位"。

### 2.4 还踩出一个顺序死循环

第 5 轮的代码是"**先检查连接性，通过了才写 mode register**"。冷启动时芯片是上电默认值，
检查必然 MISMATCH → **永远走不到写入那一步** → 芯片永远停在错的 latency 上 → 永远 MISMATCH。

**IDF 的顺序是反的**（`esp_psram_impl_enable()`）：

```
s_init_psram_mode_reg(...)        // 先写
s_check_psram_connected(...)      // 再检查
s_get_psram_mode_reg(...)         // 再读回
s_config_mspi_for_psram()         // 最后才配 DDR/8线/AXI
mspi_timing_psram_tuning()
```

而且 `s_init_psram_mode_reg()` 做的是**读-改-写**（读 MR0+MR1 和 MR4+MR5 各 16 位，
只改指定字段，其余原样写回）——一旦确认"读是准的"，读-改-写就是安全的。

### 2.5 顺手修掉的另一个位域 bug

`psram_density()` 原来把 16 位读进 `uint16_t` 再 `>>5` 取 density，
结果低位是 MR2、高位是 MR3，**把 16MB 读成了 64MB**，于是 MMU 映了 1024 页 64MB
（超出物理容量，读回来全是垃圾）。
正确写法是**只读 8 位**，density = `mr2 & 0x7`（实测 `MR2=0xdd`，`0xdd & 7 = 5` = 16MB）。

---

## 3. 真凶二：cache 写外部存储 = store access fault

> ✅ **本节所写的"未解"在 §12 找到了答案：真凶是 PMA（CPU 的自定义 CSR）。**
> 下面保留当时的现场与排除清单 —— 那份清单本身就是最好的证据：
> **凡是能 dump 出来的寄存器全都对得上**，所以差异必然在"dump 不到"的地方。

### 3.1 现象

mode register 修好之后，参考字回读 OK 了，`config_mspi_for_psram()` 17 步也跑完了，
但 MMU 映射后的读写自检 **64/64 全异常**：

```
[psramLL] 50000000: 读OK(值 5a6b7c8d) 写异常
[psramLL] 50400000: 读OK(值 aabaaaaa) 写异常
[psramLL] 40000000: 读OK(值 0addbad0) 写异常       ← flash 映射窗口
[psramLL] 70000000: 读OK(值 00000000) 写异常       ← 非 cache 别名，压根没映射
```

`mcause = 0x30000007`（code 7 = store/AMO access fault），`mtval = 0x50000000`。
**所有外部存储窗口：读都正常，写一律异常。**

### 3.2 关键前提：读是**真缓存命中**，不是旁路

用 `mcycle` 量"反复读同一个字 256 次"：

| 目标 | 周期/次 |
|---|---|
| 内部 RAM（基准） | 5 |
| flash 映射窗口 `0x40000000` | **4** |
| PSRAM 窗口 `0x50000000` | **4** |

4 拍/次不可能是"每次都下到 MSPI"（200MHz 下走一次总线是几百拍）。
**所以 D-cache 是开着的、在工作的、而且 PSRAM 读是 L1 命中。**

这条判据很重要：IDF 自己注释里写着
`CACHE_LL_ENABLE_DISABLE_STATE_SW = 1 // There's no register indicating cache enable/disable state`，
**没有任何寄存器能告诉我们 cache 开没开**，只能这样实测。

### 3.3 一个必须避开的坑：不要用 flash 窗口当"可写对照组"

第一版探测对**所有**地址都做「读一次 + 写一次」，结果 `0x40000000` **写也异常**，
差点把结论带偏。

**flash 映射窗口本来就是只读的**——往 mmap 出来的 flash 地址写，IDF 也一样是
`Store access fault`（工作区 AGENTS.md §4.16 有实测记录：`MCAUSE=0x7`、`MTVAL=0x40040000`）。
所以：**"写 flash 窗口异常"是正常的，"写 PSRAM 窗口异常"才是问题**，
两者必须分开量、分开解读。

### 3.4 逐项排除清单（都做了，都有证据）

| 候选 | 结论 | 证据 |
|---|---|---|
| MMU 页表项写错 | ❌ 排除 | 首项回读 `00000c00` = `BIT(11) VALID \| BIT(10) ACCESS_PSRAM \| paddr 0`，与 IDF **逐位一致** |
| ROM 的 `Cache_PSRAM_MMU_Set` 不好使 | ⚠️ 确认 | 它返回 0 但**什么都不生效**；改用 IDF 的 `mmu_ll_*`（`mmu_hal_map_region` 的全部内容就是那个循环） |
| PMP 拦住写 | ❌ 排除 | `pmpcfg0..3` 与 `pmpaddr0..15` **全 0**（M 模式无匹配项 = 不限制） |
| MSPI 的 PMS 权限段 | ❌ 排除 | `SPI_SMEM_S_PMS0..3` attr 全 `0x1b`（RD/WR 都允许），`PMS_REJECT` = 0 |
| MSPI 报了总线错 | ❌ 排除 | `SPI_MEM_S_INT_RAW = 0x18`（只有 SLV_ST_END/MST_ST_END，无错误位），与 IDF 相同 |
| AXI 接口被关 | ❌ 排除 | `mem_cache_fctrl.axi_req_en=1`、`close_axi_inf_en=0` |
| cache 时钟被门控 | ⚠️ 补上了，但不是原因 | `HP_SYS_CLKRST.cache_ctrl0` 的 4 个 `force_on` 位（IDF 的 `cache_ll_clk_init()`）原来确实是 0，已补；补完值 `0x156db` 与 IDF 一致，**写仍然异常** |
| cache 在复位 / 时钟关 | ❌ 排除 | 同上寄存器：`dcache_clk_en=1`、`dcache_rst_en=0`、`icache0/1_clk_en=1` |
| CPU/MEM/SYS/APB 分频不对 | ❌ 排除 | 与 IDF 真值机逐字段一致 |
| CPU 时钟太低 | ❌ 排除 | 已是 320MHz |
| 时序 / tuning 没做 | ❌ 排除 | IDF 原版 `esp_psram_impl_enable()` 里 tuning 只影响**采样点精修**；且它自己写参考数据走的是原始事务，与 cache 写路径无关 |
| **342 个寄存器的机械对差** | ❌ 无实质差异 | 见 §4 |

**还剩一个没用上的官方线索**：cache 自带"访问失败"记录寄存器
（`CACHE_L1_DCACHE_ACS_FAIL_ADDR_REG = 0x2c000238`、
`CACHE_L1_DCACHE_ACS_FAIL_ID_ATTR_REG = 0x2c000234`，还有对应的
`_INT_ENA/_RAW/_ST` 和 `CACHE_L1_CACHE_ACS_FAIL_CTRL_REG`）。它们能给出**失败访问的地址和
属性/ID**——下一轮应该从这里下手（本轮的寄存器块 dump 只覆盖到 `0x000..0x040`，没打到它们）。

---

## 4. 方法论：**拿 IDF 当"真值机" + 机械对差**（本轮唯一收敛的手段）

盲猜寄存器猜了十几轮都不收敛，最后是靠这个办法把范围从"整个芯片"缩到"没有差异"。

### 4.1 做法

1. **建一个 IDF 探针工程** `projects/psram_probe`（`sdkconfig` 直接从 `s31_membench` 拷，
   保证 PSRAM 配置一致）。它做的事：
   - 证明自己**能写 PSRAM**（`heap_caps_malloc(MALLOC_CAP_SPIRAM)` → `pa[i] = ...` → 逐字对）
   - 量 `0x50000000` 反复读的周期数（证明走不走 cache）
   - 把关心寄存器**整块**按固定格式打出来：

     ```c
     printf("R %08x %08x\n", addr, value);     // 只打非 0，diff 更干净
     ```

     覆盖：`CACHE`(0x2c000000) 0x000-0x040 / 0x3d0-0x3e0、
     `HP_SYS_CLKRST`(0x20587000) 0x000-0x080、
     `PSRAM_MSPI0`(0x20502000) / `PSRAM_MSPI1`(0x20503000) / `FLASH_SPI0`(0x20500000)
     各 0x000-0x800、PMP cfg/addr、PSRAM MMU 页表项。

2. **裸机 bootloader 侧用完全相同的格式**打同一批寄存器。
   （当时写在 `bsp/s31_psram_ll.c` 的 `regdump_block()` / `regdump()` 里；
   **那个文件在第 8 轮整理时删掉了** —— 它只是排查脚手架，结论留下、代码不留。
   现在两侧的 dump 结果存在 **[captures/](captures/)**，可以直接复核。）

3. 两边日志各自抽出 `^R ` 行，做集合 diff（有无 + 值不同）。
   原始证据：[captures/build_probe_idf.reg](captures/build_probe_idf.reg)（IDF 侧 320 个）
   + [captures/build_boot.reg](captures/build_boot.reg)（裸机侧 290 个）。

### 4.2 结果

IDF 的 PSRAM 指针是 **`0x50000984`**——**就是我们用的 `0x50000000`**，
它 `self-test: PASS (0 wrong)`。所以两边是**同一个地址范围、同一批寄存器**。

第一轮 diff 后（342 个寄存器）剩下的差异只有：

| 地址 | IDF | 我们 | 判读 |
|---|---|---|---|
| `2c000000` `CACHE_L1_ICACHE_CTRL` | `00000200` | `00000000` | bit9 = `l1_icache_undef_op` 字段（值 2）。**试过补上，无效** |
| `2c000004` `CACHE_L1_DCACHE_CTRL` | `00000200` | `00000000` | 同上 |
| `2c0003d8` `CACHE_TRACE_ENA` | `00000001` | 0 | IDF 的 cache trace 功能。**试过补上，无效** |
| `20502048` `SPI_MEM_S_SRAM_DRD_CMD` | `f0000000` | 0 | 寄存器自称"仅供内部调试"。**试过补上，无效** |
| `205020c0` `SPI_MEM_S_INT_ENA` | `0c0003e0` | 0 | IDF 开了 MSPI 中断，与写路径无关 |
| `20502380` MMU item **index** | `000000ff` | 0 | **假差异**：该寄存器**读回来恒为 0**（不回读），别拿它当证据 |

后续放大 dump 范围后又发现：
`HP_SYS_CLKRST` 的 `HPCORE1_CTRL0(0x20)` / `CRYPTO_CTRL0(0x58)` / `FLASH_CTRL0(0x64)` 有差异
（core1 时钟、加解密、flash 时钟，都与 PSRAM 写无关），
以及 `PSRAM_MSPI1` 里 IDF tuning 扫描留下的残值（`0x305c..0x3094`）、
flash MMU 的残值——都不相关。

> 📁 **上面这些差异的原始 dump 存在 [captures/](captures/)**（两个 `.reg` 文件），
> 里面还写清了怎么一条命令自己复核（实测：284 个两边都有、26 个值不同、
> 6 个只在裸机侧、36 个只在 IDF 侧）。

**结论：能读到的寄存器里没有差异，写仍然异常。**
⇒ 剩下的可能性只有 ① 只写寄存器/一次性锁存状态（diff 看不到）② cache 内部行为。

### 4.3 这个方法的适用边界

- ✅ 适合"同一颗芯片、IDF 能跑、我不能"的一切问题——**先让 IDF 当基准，再机械 diff**
- ⚠️ **看不到写-only / 只锁存一次的寄存器**。所以"diff 无差异"≠"状态一致"
  （PMA 就是活例子：它是 CSR，连"寄存器"都不是，永远 diff 不到）
- ⚠️ 别忘了**独立第三方佐证**：当时还拿了一份 Chalandi 的裸机 P4 工程
  （`tmp/p4_nosdk/`，**临时目录已删**）——它的 `Code/SBL/mmu.c` 独立证实了
  MMU 页表项格式 `(1<<11)|(1<<10)|(paddr>>16)`，并写着
  *"the cache is enabled in the bootROM no need to re-initalize it"*

---

## 5. 交付的写路径：原始 MSPI 事务（**不是权宜之计**）

既然 cache 写不通，PSRAM 的写就走**不过 cache 的原始 MSPI 事务**。

**这不是我发明的绕路——IDF 自己往 PSRAM 写参考数据就是这么干的**
（`components/esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/mspi_timing_by_dqs.c`）：

```c
while (len) {
    int len_to_send = MIN(len, PSRAM_CTRLR_LL_FIFO_MAX_BYTES);   // 64 字节
    psram_ctrlr_ll_common_transaction(PSRAM_CTRLR_LL_MSPI_ID_3,
        AP_..._SYNC_WRITE, ..., w_ptr, len_to_send * 8, NULL, 0, false);
    w_ptr += len_to_send;  addr += len_to_send;  len -= len_to_send;
}
```

本项目的实现（`bsp/s31_psram_ll.c`）用的是**同一个** `psram_ctrlr_ll_common_transaction`：

```c
#define PSRAM_RAW_CHUNK  64u      /* = PSRAM_CTRLR_LL_FIFO_MAX_BYTES */

void s31_psram_write_raw(uint32_t paddr, const void *buf, uint32_t len);
void s31_psram_read_raw (uint32_t paddr, void *buf, uint32_t len);
void s31_psram_invalidate_cache(uint32_t vaddr, uint32_t len);
```

**三个必须注意的点**：

1. ⚠️ **必须在 `config_mspi_for_psram()` 之后调用**——那一步才把控制器切成 DDR + 8 线模式，
   dummy 长度也是它设的。
2. ⚠️ 地址是 **PSRAM 物理地址**（= 虚拟地址 `- 0x50000000`）。本项目 MMU 是 1:1 映射的
   （`vaddr = 0x50000000 + i*64K` → `paddr = i*64K`）。
3. ⚠️ 写完**必须让 cache 里对应的行失效**（`Cache_Invalidate_Addr(CACHE_MAP_L1_DCACHE, vaddr, len)`），
   否则紧接着的 cache 读会拿到陈旧内容。

**验证（交叉，不是自证）**：

```
[psramLL] ★ 原始写 + 原始读回: OK (0/256 错)       ← 同一条路写、同一条路读
[psramLL] ★ 原始写 + cache 读回: OK (0/256 错, 异常 0 次)   ← 写走 raw、读走 cache
[psram]   4KB pattern test: PASS (0 errors)
```

**已经接到 App 搬运上**（`app/app_image.c`）：PSRAM 段（普通段和 bss 段都）
走 `s31_psram_write_raw`，写完 `s31_psram_invalidate_cache`；
`bsp/s31_psram.c` 的 `s31_psram_crc_test()` 也改成了"raw 写 + cache 读"。

---

## 6. 当时的已知限制 & 待办

> ⚠️ **本节写于 §12 之前，里面两条限制后来都被推翻了**（可写数据能放 PSRAM 了、
> "全部放 PSRAM"也做到了）。保留原文是为了记录当时的判断。

### 6.1 ⚠️ 设计约束：App 放 PSRAM 时，可写数据必须留在内部 RAM

> ❌ **已被 §12 推翻**：PMA 修好之后，可写数据同样可以放 PSRAM。

因为"cache 写 PSRAM"不通，**App 运行时不能往 PSRAM 里的变量写**。所以 App 链接脚本要：

| 内容 | 放哪 | 为什么 |
|---|---|---|
| `.text` / `.rodata` | **PSRAM** ✅ | 只读，走 cache 读，正常 |
| `.data` / `.bss` / 栈 / 堆 | **内部 RAM** ✅ | 运行时会被写 |

这仍然满足项目目标（"LMA 在 flash、VMA 在 PSRAM"），只是把可写部分摘出来。

### 6.2 待办

> ⚠️ 下面这两条是**当时**的待办，现在都已经做完了（`s31_app` 的链接脚本早就改过，
> 最后落到"LMA 在 flash、VMA 在 PSRAM"的经典模型，见 [README.md](README.md) §3.0）。

- [ ] 调 `projects/s31_app/bsp/linker.ld`：`.text`/`.rodata` → PSRAM，可写数据 → 内部 RAM
- [ ] 跑通完整链路：拖 `app.bin` → 写 flash → 复位 → bootloader 搬进 PSRAM → 跳转 → App 运行
- [x] ~~把 `bsp/s31_psram_ll.c` 里那一大堆诊断（`cache_probe` / `dcache_count` /
      `cache_speed` / `regdump` / `pmp_dump` / A/B/C 试验）收敛到一个开关后面~~
      → **已彻底删掉**，见 §11
- [ ] 继续挖 cache 写：从 `CACHE_L1_DCACHE_ACS_FAIL_ADDR/ID_ATTR` 下手（§3.4 末尾）
- [ ] 清掉 IDF 探针残留在 flash `0x8000`（分区表）和 `0x10000`（psram_probe app）的东西

---

## 7. 复现 / 验证命令

```powershell
# 编译 + 烧录 + 读日志（会打印上面那些 ★ 行）
cd boot_msc_s31
pwsh -File tools\build.ps1                 # 加 -Rebuild 强制全编，-BuildOnly 只编不烧
pwsh -File tools\build.ps1 -ReadSeconds 12 # 看日志久一点

# 只看关注的行
Select-String -Path build\boot.log -Pattern '★|参考字|psram\]|Z:'

# IDF 真值机（PSRAM 寄存器基准）
#   在 ESP-IDF 工程里烧一份对照固件，把它的 PSRAM 寄存器 dump 出来对比：
#   build\bootloader\bootloader.bin@0x2000 + partition-table.bin@0x8000 + app.bin@0x10000
#   （烧完之后记得把 boot_msc_s31 烧回去，它才是主角）
```

**通过时的日志长这样**（照这个对）：

```
[psramLL] 写前 MR0=72 MR1=8d(vendor=0d=AP) MR4=30 MR8=6b
[psramLL] 参考字: 写 5a6b7c8d 读 5a6b7c8d -> OK
[psramLL] MMU: 256 页 @50000000 (entry 0) 首项=00000c00 VALID
[psramLL] ★ 原始写 + 原始读回: OK (0/256 错)
[psramLL] ★ 原始写 + cache 读回: OK (0/256 错, 异常 0 次)
[psram]   4KB pattern test: PASS (0 errors)
[usb] ready: plug the USB-HS cable, drag app.bin onto the S31-BOOT drive
```

---

## 8. 🚑 排查过程中把板子搞"砖"了一次（以及怎么救）

**症状**：控制台 0 字节输出；`esptool` 报
`PermissionError(13, '设备不识别')`；COM43 还在列表里但打不开。

**原因**：那次崩溃进了异常停机循环，而停机循环里**毫无间隔地狂灌 USB-Serial/JTAG 的 FIFO**
→ Windows 直接把这个 USB 设备判成"无法识别"。

**救法**（不需要断电！）——内置 JTAG 不受 USB-Serial/JTAG 死活影响：

```powershell
$ocd = "$env:USERPROFILE\.espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\bin\openocd.exe"
$s   = "$env:USERPROFILE\.espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\share\openocd\scripts"
& $ocd -s $s -f board/esp32s31-builtin.cfg -c "init" -c "reset run" -c "shutdown"
```

**已经做的加固**：

1. 停机循环里加了粗延时（`for (volatile uint32_t d = 0; d < 40000u; d++) {}`），不再灌爆 USB
2. 裸机里**任何"预期可能异常"的探测都必须开 `g_probe_guard`**
   （`bsp/trap_handler.c`）——守卫会跳过那条指令并计数，不让整个固件停机
3. 守卫现在也跳过 **code == 2（非法指令）**，所以可以放心用 `csrr` 去读可能未实现的 CSR
   （PMP dump 就是这么读的）

**通用规矩**：**在 `PSRAM` / `MMU` / `cache` 还没确认可用之前，新加的探测代码一律先想
"它异常了会怎样"**——裸机没有 MMU 保护，一条 store 就是板子失联。

---

## 9. 顺带修的基础设施（这一轮加的）

| 位置 | 改动 | 为什么 |
|---|---|---|
| `bsp/s31_psram.c` | 全部初始化 + MMU + 原始读写（**单文件 609 行**） | 第 8 轮整理后 |
| `bsp/sdkconfig.h` | 极简版（只给 IDF 的 LL 头用） | `hal/assert.h` 要 `#include "sdkconfig.h"` |
| `bsp/mini_libc.c` | 补了个打印后停机的 `abort()` | 配合上面的断言级别=1 |
| `bsp/linker.ld` | `PROVIDE(EFUSE = 0x20715000)` | `hal/mmu_ll.h` 要读 efuse 判 flash 加密 |
| `bsp/linker.ld` | `PROVIDE(CACHE = 0x2C000000)` | 按字段访问 `CACHE.l1_*_ctrl` |
| `tools/build.ps1` | 只保留 10 个必要的 IDF include 根 | 第 8 轮整理后 |
| `bsp/trap_handler.c` | 守卫支持 code==2；停机循环加延时 | 见 §8 |
| `projects/psram_probe/` | **新增** IDF 真值机工程 | §4 |

---

## 10. 第 7 轮：App 真的跑进 PSRAM 了（以及又排除掉一批）

### 10.1 ✅ 交付：`.text`/`.rodata` 在 PSRAM，从 PSRAM 取指运行

板上实测（拖入 4486 B 的 `app.bin`，自动复位跳转后）：

```
[app] .text   50000000..50000ad8  (2776 B)      ← 在 PSRAM
[app] .rodata 50000ad8..50000f7a  (1186 B)      ← 在 PSRAM
[app] .data   2f060148..2f060148  (0 B)
[app] .bss    2f060150..2f061360  (4624 B)      ← 在内部 RAM
[app] &app_main = 500007ac                      ← 代码在 PSRAM
[app] &s_banner = 50000f5c                      ← 常量在 PSRAM
[app] sp = 2f079fd0                             ← 栈在内部 RAM
[app] 结论: OK —— 代码在 PSRAM、可写数据在内部 RAM（就是从 PSRAM 取指跑起来的）
[app] alive #1 ... #22   t=21ms..21046ms        ← 稳定跑满 21 秒
```

**这证明了三件事**：
1. bootloader 能把段用**原始 MSPI 事务**写进 PSRAM（app_image.c 的 PSRAM 分支 + cache 失效）；
2. **从 PSRAM 取指能跑**（I-cache + IBUS1 + PSRAM MMU 这条路是通的）；
3. `.rodata` 从 PSRAM 读也没问题（D-cache 读）。

**⇒ "LMA 在 flash、VMA 在 PSRAM" 这个需求本身已经达成。**

### 10.2 ⚠️ 但"全部放 PSRAM（不要 SRAM）"做不到 —— 卡在同一处

> ❌ **已被 §12.4 推翻**：PMA 修好之后真的做到了，连栈都在 PSRAM 里。

App 一运行就要写 `.data`/`.bss`/栈，而**运行时写 PSRAM 会 store access fault**（§3）。
所以可写数据必须留在内部 RAM。这不是偷懒，是硬约束。

### 10.3 本轮新排除的（别再查一遍）

| 候选 | 结论 | 证据 |
|---|---|---|
| **CPU_APM 区域过滤器** | ❌ 排除 | 它是**真的开着**：`REGION_FILTER_EN=0x01`，region0 覆盖 `0x40000000..0x5fffffff`（= flash 映射窗口 + PSRAM 窗口），`ATTR=0x7000` 解出来是 `tee_x/w/r=7` 而 **`r0/r1/r2` 三种 REE 模式的 R/W/X 全是 0**；`M0_EXCEPTION_INFO1` 也记了被拒地址。**但把 FILTER_EN 清 0、把 region0 attr 改成 0x7777 之后，写照样异常。** |
| **HP_APM / HP_MEM_APM / TEE** | ❌ 排除 | 都 dump 了：HP_APM region 全 `ffffffff` + attr `0x7000`（同样"只给 TEE"），HP_MEM_APM region 全是内部 RAM `2f000000..2f07ffff`。都不是 PSRAM 窗口 |
| **cache 自己的"访问失败"判定** | ❌ 排除 | `CACHE_L1_CACHE_ACS_FAIL_CTRL_REG = 0x13`（bit4 = dcache check mode 1，"失败会传播给请求"）看着很像凶手，**但把失败记录器清干净、让 INT_ENA 打开、再做一次 store —— `D_ADDR/ID_ATTR/RAW/ST` 全是 0，cache 压根没记这笔** |
| `SPI_MEM_S_CTRL1`（AR/AW SIZE 支持位等） | ❌ 排除 | `0x2ec00000`，与 IDF **逐位一致**（`AW/AR_SIZE0_1_SUPPORT_EN` 都=1，`CLOSE_AXI_INF_EN`=0） |
| **"IDF 能写 PSRAM 是假象"** | ❌ 假设被推翻 | 见 §10.4 |
| 核心 1 干扰 | ❌ 排除 | `HPCORE1_CTRL0 = 4` → `reg_core1_global_rst_en=1`，**core1 一直被摁在复位里**（IDF 的 app 是 3 = 放出来跑）。它不可能干扰 |
| `0x50000000` 这个地址本身有问题 | ❌ 排除 | IDF **在 0x50000000 上写得进去**（见 §10.4） |

### 10.4 ⚠️ 一个差点把自己带偏的测试陷阱（方法论）

我一度怀疑"**IDF 能写 PSRAM 也是假象**"（store 被静默丢弃、紧接着的读命中 cache，于是"自检 PASS"）。

第一版判据用了 `Cache_Invalidate_Addr()` —— 结果 **16384/16384 全错**，看起来坐实了"写是假的"。

**但这个判据本身是错的**：`Cache_Invalidate_Addr` 是**丢弃脏行、不回写**，
用它验"数据有没有落到 PSRAM"，等于亲手把数据扔掉再去找。
（顺带解释了为什么是"全错"而不是"最后 16KB 错"：`CACHE_L1_DCACHE_CACHESIZE` = 64K，
我刚好写了 64KB，**整块都还在 cache 里、一行都没被挤出去**。）

改用 **`Cache_WriteBack_Invalidate_Addr()`**（先写回再失效）之后：

```
PSRAM 64KB write: 热读错 0 / 写回+失效后错 0  -> 写是真的落到 PSRAM 了
  PSRAM @50000000: OK    @50000100: OK    @50000980: OK    @50010000: OK
  PSRAM @50080000: OK    @500ffc00: OK    @50100000: OK    @50400000: OK
```

**IDF 在 0x50000000 上写得进去，写得到 PSRAM。** ⇒ **这个 bug 是我们裸机环境的，不是硅片的，也不是地址的。**

> **教训**：验证"写有没有落地"必须用 **write-back + invalidate**；
> plain invalidate 会给出**假阴性**。而且测的数据量要**远大于 cache**，
> 否则整块都在 cache 里，一行都不会被挤出去。

### 10.5 现在还站得住的差异（下一步就查这几个）

> ⚠️ **本小节是当时的"待查清单"，§12 已经给出答案（PMA）**；留着当推理记录。

1. **环境**：我们的代码跑在 **IRAM**（0x2F000000），IDF 的 app 跑在 **flash 映射窗口**。没验证过这是否影响 store 通路。
2. **我们从来没跑过 `mspi_timing_psram_tuning()`**（仍是 stub）。它是 IDF 在 `s_config_mspi_for_psram()` 之后的必经步骤，
   里面会**在 MSPI3 上做原始事务写 PSRAM 参考数据**，再按扫描结果写回 timing 寄存器。
   我们**也**在 MSPI3 上做过原始事务（参考字检查），但**没有它后面那段"按扫描结果回写"**。
   ⚠️ 值得一试的假设：**MSPI3 的原始事务把 MSPI2(AXI/cache 侧) 的某些状态带歪了，而 tuning 的最后一步会把它恢复**。
3. **A/B 实验建议**（最省时间的两条）：
   - 在 IDF 的 `psram_probe` 里**先做一次 MSPI3 原始事务写**，再试 cache 写 —— 如果 IDF 也因此写不进去，就锁定"原始事务污染了 AXI 侧"。
   - 反过来：在裸机里**先不做任何 PSRAM 数组原始写**（连参考字检查都跳过），直接映射 MMU + cache 写，看能不能写。

### 10.6 本轮顺手修掉的两个工程问题

1. **`mkapp.py` 必须真正按段打包**：原来用 `objcopy -O binary` 一把梭，
   而它会把镜像**从最低 VMA 拍平到最高 VMA** —— `0x2F060000` 和 `0x50000000` 之间隔着 ~550MB，
   拍出来就是一坨 550MB 的"payload"。现已改成**直接从 ELF 按段取内容**，
   按 VMA 连续性合并（空隙 ≤64B 用 0 填），产出：
   ```
   [0] COPY IRAM  vma=0x2f060000 size=  332  <- .boot+.clic_entry+.sdata
   [1] COPY PSRAM vma=0x50000000 size= 3962  <- .text+.rodata
   [2] ZERO IRAM  vma=0x2f060150 size= 4624  <- .bss+.sbss
   ```
2. **`s31_psram_invalidate_cache()` 必须 I/D 都失效**（`CACHE_MAP_MASK`）：
   App 的 `.text` 也在 PSRAM，只失效 D-cache 的话，跳过去可能执行到陈旧指令。

---

## 11. 第 8 轮：把这一摊整理成**一个文件**

整理前 PSRAM 相关代码散在 `s31_psram_ll.c`（1200 行，一半是诊断）、
`s31_psram.c`（30KB，90% 死代码）、`idf_psram_shim.c`、以及
`bsp/idf_port/`（**19.9MB / 535 个文件的 IDF 头文件倾倒场**，单个文件 1.7MB）。

### 11.1 结果

```
bsp/:  19.9 MB / 535 个文件  ->  0.2 MB / 16 个文件
bootloader app.bin:  47.5 KB  ->  39.2 KB
```

**PSRAM 现在只有 `bsp/s31_psram.c`（609 行）**，对外 5 个函数：

```c
int      s31_psram_init(void);                                  // 0 = 成功
void     s31_psram_write_raw(uint32_t paddr, const void *buf, uint32_t len);
void     s31_psram_read_raw (uint32_t paddr, void *buf, uint32_t len);
void     s31_psram_invalidate_cache(uint32_t vaddr, uint32_t len);
uint32_t s31_psram_crc_test(uint32_t base, uint32_t bytes);      // 0 = 通过
```

删掉的东西：所有诊断（`cache_probe` / `dcache_count` / `cache_speed` / `regdump` /
`pmp_dump` / `psram_ana_dump`）、A/B/C/D/E 那一串试验、IDF 原版实现那条对照路径
（`s31_psram_idf_init` + `idf_psram_impl.c` + `idf_psram_shim.c`）、
早期手写版的死代码、以及 `tools/gen_sdkconfig_h.py`（只为生成那份 84KB 的 sdkconfig.h）。

### 11.2 「太复杂的宏展开」是怎么消掉的

IDF 的 `psram_ctrlr_ll_xxx()` 是这么个套壳宏：

```c
#define psram_ctrlr_ll_enable_module_clock(...) do { \
        (void)__DECLARE_RCC_ATOMIC_ENV; \
        _psram_ctrlr_ll_enable_module_clock(__VA_ARGS__); \
    } while(0)
```

逼得调用方到处 `#define __DECLARE_RCC_ATOMIC_ENV 0`，还带一堆
`statement with no effect` 警告。**但只有 5 个函数有这种壳**
（`enable_core_clock` / `enable_module_clock` / `reset_module_clock` /
`select_clk_source` / `set_core_clock_div`）—— 直接调它们的**下划线版本**即可，
那是不带宏壳的普通 `static inline` 函数。**IDF 自己的二级 bootloader
（`bootloader_esp32s31.c`）就是直接调下划线版本的。**

现在整个文件里没有一处 `__DECLARE_RCC_ATOMIC_ENV`、没有一处 `(void)0`。

### 11.3 但 LL 头文件本身**留着**

```c
#include "hal/psram_ctrlr_ll.h"   /* IDF */
#include "hal/mspi_ll.h"          /* IDF */
#include "hal/mmu_ll.h"           /* IDF */
```

**为什么不手写寄存器**：本项目为此付过两次学费 ——
`LP_AONCLKRST_MSPI_DIV.FB_DIV` 实际在 bit[7:3] 而我写在 bit[4:0]；
AP octal PSRAM 的 MR0/MR4/MR8 三个位域我全按印象拍错。两次都是
"寄存器抄错、不报错、只是行为不对"，各花掉一整轮。所以按 IDF 的**字段名**写，
位号交给 IDF 的头文件。

`bsp/idf_port/` 那份副本也因此删掉了 —— 直接用 IDF 源码树里的原始头文件，
`tools/build.ps1` 里只列真正需要的 10 个 include 根（含
`esp_hal_mspi/esp32s31/include`、`hal/esp32s31/include`、`soc/esp32s31/register`）。

顺带：IDF 的 `hal/assert.h` 要 `#include "sdkconfig.h"`，而本工程不链 IDF。
所以手写了一个**极简 `bsp/sdkconfig.h`**（只有 3 行，定义
`CONFIG_HAL_DEFAULT_ASSERTION_LEVEL 1`），并在 `mini_libc.c` 里补了个
打印一行后停机的 `abort()` —— 万一 LL 函数被传了非法参数，会响亮地停住，
而不是掉进 `__builtin_unreachable()` 的未定义行为。

### 11.4 整理后的日志（一眼看得懂，也就 6 行）

```
[psram] === 初始化（16MB 8 线 DDR @200MHz）===
[psram] 芯片 = AP 系列 (vendor 0x0d)
[psram] mode reg: MR0=30 MR4=20 MR8=0b（期望 30/20/0b）
[psram] 参考字: 写 5a6b7c8d 读 5a6b7c8d -> OK
[psram] MMU: 256 页 @50000000 -> PSRAM 物理 0（首项 00000c00）
[psram] 自检: 原始写+原始读 OK / 原始写+cache读 OK -> PSRAM 可用
[psram] pattern test: PASS (0 errors)
```

**整理后完整回归验证通过**：App（`.text` 在 PSRAM）照常从 PSRAM 取指运行，
`[app] alive #1 … #10` 稳定。

---

## 12. 第 9 轮：★★★ **真凶是 PMA** —— "cache 写 PSRAM 全挂"彻底解决

### 12.1 答案

**这颗 CPU 有 PMA（Physical Memory Attribute），IDF 把它实现成自定义 CSR**
（`components/riscv/include/riscv/csr.h`）：

```c
#define CSR_PMACFG(i)   0xBC0 + i        // i = 0..15
#define CSR_PMAADDR(i)  0xBD0 + i
#define PMA_EN     BIT(0)     使能
#define PMA_W      BIT(3)     ★ 写
#define PMA_X      BIT(2)     执行
#define PMA_R      BIT(4)     读
#define PMA_L      BIT(29)    锁定
#define PMA_NAPOT  0xC0000000
#define PMA_SHIFT  2
// PMAADDR 存的是 NAPOT 掩码形式：((ADDR | ((SIZE>>1)-1)) >> 2)
```

本板 ROM 留下的现场（实测 dump）：

| 条目 | cfg | 区间 | 权限 |
|---|---|---|---|
| PMA12 | `C0000019` | `[0x2E000000, +32KB)` LP/RTC RAM | R+W |
| PMA13 | `C0000015` | `[0x2F800000, +512KB)` ROM | R+X |
| PMA14 | `C000001D` | `[0x2F000000, +512KB)` IRAM | R+W+X |
| **PMA15** | **`C0000015`** | **`[0x40000000, +512MB)` 外部存储** | **R+X ← 没有 W** |

`[0x40000000, +512MB)` 正是 cache 的外部存储虚拟窗口 —— **flash 映射窗口和 PSRAM
窗口都在这一段里**。ROM 把整段标成只读，于是：

- 读全对 ✅
- 能执行 ✅（App 的 `.text` 就是从 PSRAM 取指跑的）
- **任何 store 一律 `store access fault`** —— 连 flash 窗口和 PSRAM 窗口的表现都
  一模一样，因为对 PMA 来说它们是同一段

**IDF 侧的权威写法**在 `components/esp_hw_support/port/esp32s31/cpu_region_protect.c`，
它自己的注释就写着：

```c
//without setting this, psram cannot be reached
PMA_RESET_AND_ENTRY_SET_NAPOT(7, SOC_EXTRAM_LOW, (SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW),
                              PMA_NAPOT | PMA_RWX);
```

而这个 `esp_cpu_configure_region_protection()` 是**二级 bootloader** 调的
（`bootloader_mem.c:63`）。**本工程的裸机 bootloader 顶掉了二级 bootloader，
所以这一步从来没人做。**

### 12.2 修法（`bsp/s31_psram.c` 的 `pma_grant_write()`）

比 IDF 更保守：**不重置整张表**（我们正从 IRAM 里跑，重置 PMA 有风险），而是
扫一遍 16 条，凡是"使能的 NAPOT 条目且区间覆盖 PSRAM 窗口"的，**补上 `PMA_W`**：

```
[psram] PMA15 [40000000,+524288KB) 补上写权限: c0000015 -> c000001d
```

调用点在 `s31_psram_init()` 的**第 ⓪ 步**（早于任何 PSRAM 写）。

### 12.3 为什么查了这么久 —— 三条教训

1. 🚨 **PMA 是 CSR，不在内存映射空间里，所以任何"寄存器对差"都看不见它。**
   本项目拿 IDF 当真值机、逐块比过 **342 个寄存器**（CACHE / HP_SYS_CLKRST /
   三个 MSPI / PMP / MMU / CPU_APM / HP_APM / TEE / PMS），**全都对得上** ——
   这个"全都对得上"本身就是最强的线索：**差异一定在非内存映射状态里**。
2. 🚨 **"能读不能写"要第一时间想到 PMA/PMP 这类权限表，而不是 cache 策略。**
   我先后排除了 MMU、PMP、CPU_APM、HP_APM、HP_MEM_APM、TEE、PMS、cache
   时钟/使能/旁路/miss 策略、MSPI 全部寄存器……才轮到 PMA。
3. ✅ **两个判别实验直接把范围砍到"CPU 核内"**：
   - **旁路 L1 D-cache 后写仍然异常**（`CACHE_L1_BYPASS_CACHE_CONF_REG.bit4`）
     → 不是 cache 的缓存策略/写分配问题；
   - 同一状态下**读照常成功**
     → 地址译码和总线都是好的，是**权限**问题。
   然后 `SOC_CPU_HAS_PMA = 1` 这条 soc_caps 就是路标。

### 12.4 结果：**App 全部放 PSRAM，一点内部 RAM 都不用**

真凶解决后，`projects/s31_app/bsp/linker.ld` 一度改成单区域全 PSRAM
（连 `.boot`/`.clic_entry`/栈都在里面）；**现在用的是"LMA 在 flash、VMA 在 PSRAM"的
经典模型**（[README.md](README.md) §3.0），因为那样镜像的初值可以留在 flash 里。
（连 `.boot`/`.clic_entry`/栈都在 PSRAM）。板上实测：

```
[app] .text   5000013c..50000c14  (2776 B)
[app] .rodata 50000c18..50001096  (1150 B)
[app] .bss    500010a0..500022b0  (4624 B)
[app] &app_main = 500008e8   &s_banner = 50001078   &s_magic = 50001098
[app] sp = 50ffffd0                                   ← 栈在 PSRAM 末尾
[app] .data magic = 0x1234abcd    .bss counter = 0    ← 可写数据在 PSRAM 里正常
[app] 结论: OK —— 代码 / 常量 / 可写数据 / 栈全部在 PSRAM，一点内部 RAM 都没用
[app] alive #1 ... #4
```

**⇒ 需求"LMA 在 flash、VMA 在 PSRAM"以最强形式达成。**

### 12.5 顺带：`s31_psram_write_raw()` 仍然留着

PMA 修好之后，"普通 store 写 PSRAM"已经可以用了，那 `write_raw` 还需要吗？

**需要，但用途变了**：
- **App 搬运**仍然用它 —— 因为它**绕过 cache**：镜像是用 SPI1 直接写进 flash 的，
  而 App 的 VMA 在 PSRAM，走普通 store 的话 cache 里可能留着上一个 App 的旧行
  （所以 `app_image.c` 是 `write_raw` + `s31_psram_invalidate_cache` 配对用的）。
- 自检 `s31_psram_crc_test()` 也仍然用它 + cache 读交叉验证 ——
  这是**唯一能证明"真的落到 PSRAM 了"而不是"只落在 cache 里"**的手法。

---

## 13. 一句话留给下一个人

> **PSRAM 的读、写、取指、装载现在全都是好的。**
> 如果没有这一路的记录，最可能被"看起来最像"的方向骗走的是这三个：
> ① **dummy 长度 / 采样相位 / timing tuning**（跟 PSRAM 初始化失败**一点关系都没有**，
>    真凶只是 mode register 的三个位域）；
> ② **cache 策略**（"能读不能写"最容易被归到这儿，真凶其实是**权限**）；
> ③ 🚨 **PMA** —— 它是 **CPU 的自定义 CSR，不在内存映射空间里**，
>    **再多的寄存器 dump 也看不见它**。IDF 的二级 bootloader 靠
>    `esp_cpu_configure_region_protection()` 打开外部存储窗口的写权限，
>    裸机 bootloader 把这一步丢了，就会得到"读得好好的、一写就 access fault"。
>
> 通则：**"所有能 dump 的寄存器都对得上"本身就是最强的线索 ——
> 差异一定在 dump 不到的地方**（CSR、PMA/PMP 这类权限表、eFuse、或者硬件状态机）。
