# bsp/idf_headers —— 冻结进来的 ESP-IDF 头文件

> **这个目录是自动生成的，别手改。**
> 重新生成：`pwsh -File tools\sync_idf_headers.ps1`

## 这是什么

`bsp/s31_rmt.c` 要用 IDF 的 RMT LL 头（`hal/rmt_ll.h`、`hal/rmt_types.h`、
`soc/rmt_struct.h`、`soc/clk_tree_defs.h`、`soc/hp_sys_clkrst_struct.h`、
`soc/hp_system_struct.h`、`soc/gpio_sig_map.h`）——**按字段名写寄存器，不手抄位号**。
本项目为"手抄位号"栽过两次（见 `boot_msc_s31/PSRAM.md` §2 §12），
所以这里的规矩是：宁可多拷几个头，也不自己拼位域。

顺带一个现实原因：**S31 是 preview target**，它的 RMT 与 IDF 老芯片不太一样
（`rmt_ll.h` 里 `RMT_LL_EVENT_TX_DONE` 的算法、内存窗口、时钟源枚举都不同），
照别的手册抄必错 —— 直接拿 IDF 自己的头最省事。

这些头会拖出一整个传递闭包（大头是 `soc/hp_sys_clkrst_struct.h`，
把每个寄存器字段都列了一遍），所以把它们**冻结进工程**，
从此编译不再需要 IDF 源码树。

**⇒ 本工程不依赖 ESP-IDF**：换台机器只要有 RISC-V 工具链就能编。

## 怎么生成的

`tools/sync_idf_headers.ps1`（与 `boot_msc_s31` 那份同源）
**不自己解析 `#include`**（那样会漏掉条件编译、以及 `#include "sibling.h"`
这种靠"同级目录"解析的形式），而是用 `gcc -M` **问编译器**：
把依赖表里所有落在 IDF 树里的头文件全部拷过来。

- 目录形状 = 每个头**相对命中它的那个 include 根**的路径
  （因为 include 字符串本身就是将来在这个扁平目录下的相对路径）；
- 顺序 = 编译器搜索 `-I` 的顺序，所以"第一个命中"和编译器看到的完全一致；
- **12 个文件 / 约 376 KB**，来源见 [`_SOURCE.txt`](_SOURCE.txt)（含 IDF 版本与 commit）。

⚠️ 脚本**只删 `*.h` 和 `_SOURCE.txt`**，不整目录删 ——
否则会把本文件（手写的）一起干掉（`boot_msc_s31` 那边踩过）。

## 什么时候需要重新同步

只有当 IDF 那边 **S31 的寄存器定义被修正** 时才需要，比如：

- S31 从 preview target 转正、寄存器头跟着改；
- 发现某个 `*_reg.h` 的位域定义有误。

那时跑一次 `sync_idf_headers.ps1`，然后**重新编译 + 上板验证**即可
（`_SOURCE.txt` 会记下新的版本与时间戳）。

## 许可

这些文件来自 [ESP-IDF](https://github.com/espressif/esp-idf)，
版权归 Espressif Systems 所有，**Apache License 2.0**。
原样复制、未做修改；每个文件里都保留着它自己的版权头。
