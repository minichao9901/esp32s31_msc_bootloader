# captures —— PSRAM"寄存器对差"的原始证据

> 这两个文件是 `boot_msc_s31` 的 PSRAM 攻坚战里，**"拿 IDF 当真值机"那一步的原始 dump**。
> 判读结论见 [../PSRAM.md](../PSRAM.md) §4；这里只说明文件本身。

## 文件

| 文件 | 谁打的 | 行数 | 格式 |
|---|---|---|---|
| `build_probe_idf.reg` | **IDF 侧** —— `projects/psram_probe`（能写 PSRAM 的探针工程）| 342 行 / R 行 320 | `R <addr> <value>`，另有 `PMPCFG`/`PMPADDR` 几行 |
| `build_boot.reg` | **裸机侧** —— `boot_msc_s31` 自己（同一个格式）| 295 行 / R 行 290 | 同上，另有 `MMUPSRAM <i> <entry>` 几行 |

地址覆盖（合并去重后 320 个）：

| 段 | 个数 | 是什么 |
|---|---|---|
| `0x2050xxxx` | 293 | `FLASH_SPI0`(0x20500000) / `PSRAM_MSPI0`(0x20502000) / `PSRAM_MSPI1`(0x20503000) 各一整块 |
| `0x20587xxx` | 23 | `HP_SYS_CLKRST` |
| `0x2c00xxxx` | 10 | `CACHE`（L1 I/D cache 控制等）|

## 机械对差的结果（可以自己复核）

```powershell
function Parse($f){ $m=@{}; foreach($l in (Get-Content $f)){
  if($l -match '^R\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)'){ $m[('0x'+$matches[1].ToLower())]=$matches[2].ToLower() } }; $m }
$b = Parse .\build_boot.reg; $p = Parse .\build_probe_idf.reg
"两边都有 $(($b.Keys|?{$p.ContainsKey($_)}).Count) 个；" +
"值不同 $((($b.Keys|?{$p.ContainsKey($_)-and $b[$_] -ne $p[$_]}).Count)) 个；" +
"只在裸机 $((($b.Keys|?{-not $p.ContainsKey($_)}).Count)) 个；" +
"只在 IDF $((($p.Keys|?{-not $b.ContainsKey($_)}).Count)) 个"
```

结果：**284 个两边都有；26 个值不同；6 个只在裸机侧；36 个只在 IDF 侧。**

差异的构成（都和"写 PSRAM"无关，逐条判读见 PSRAM.md §4.2）：

- `0x2050305c..0x20503094` / `0x2050345c..0x20503494`（共 32 个，**只在 IDF 侧**）
  —— IDF 的 `mspi_timing_psram_tuning()` 扫描时留在 PSRAM_MSPI1 里的残值；
- `0x20502380`（只在 IDF 侧）—— MMU item **index** 寄存器，**读回来恒为 0**，是假差异；
- `0x205020c0`（只在 IDF 侧）—— `SPI_MEM_S_INT_ENA`，IDF 开了 MSPI 中断，与写路径无关；
- `0x20587020 / 0x20587058 / 0x20587064` —— `HP_SYS_CLKRST` 的 `HPCORE1_CTRL0` /
  `CRYPTO_CTRL0` / `FLASH_CTRL0`（core1 时钟、加解密、flash 时钟），与 PSRAM 无关；
- `0x20500008/10/1c/20/c8/380` + `0x205004xx`、`0x20503004/1c/20/28/58` + `0x205034xx`
  —— 两套 MSPI 寄存器的成对差异，都是 IDF tuning / 时钟配置的产物。

> ⚠️ **这两份不是"第一轮"的 dump**，是后来**放大 dump 范围之后**的版本 ——
> 所以 `0x2c000000`/`0x2c000004`/`0x2c0003d8`（cache ctrl / trace）在这两个文件里**值是一样的**
> （PSRAM.md §4.2 那张表里列的差异属于更早的一轮，当时"试过补上、无效"）。

## ★ 这两个文件真正的价值

不是"哪些寄存器不一样"，而是**"能读到的寄存器里，没有一处能解释写为什么挂"**。
于是范围被压缩到两种可能：

1. **只写 / 一次性锁存的状态**（dump 看不到）；
2. **cache 内部行为**。

最后答案是第 ①类的极端情况：**PMA** —— 它是 **CPU 的自定义 CSR**，
连"内存映射寄存器"都不是，**任何寄存器 dump 都不可能看到它**。
（详见 PSRAM.md §12。）

⇒ 方法论：**"所有能 dump 的寄存器都对得上"本身就是最强的线索 ——
差异一定在 dump 不到的地方。**
