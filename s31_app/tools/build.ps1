#===========================================================================
# build.ps1 -- 构建裸机 App 并打包成 bootloader 能拖拽烧录的 app.bin
#
#   pwsh -File tools\build.ps1              # 编译 + 打包 -> build\app.bin
#   pwsh -File tools\build.ps1 -Drag        # 打包完直接拷进 S31-BOOT 盘
#   pwsh -File tools\build.ps1 -Rebuild     # 全量重编
#
# 产物 **build\app.bin 就是"拖进 S31-BOOT 盘"的那个文件**：
# 纯 `objcopy -O binary` 的输出，没有自定义头/段表（bootloader 按"开头不是 'S31A'"
# 识别成扁平镜像）。-DragFlat 是老选项名，保留兼容。
#===========================================================================

param(
    [switch]$Rebuild,
    [switch]$Drag,
    [switch]$DragFlat,
    [string]$Drive = '',
    # 拖哪个 bin：app.bin（扁平，默认）/ app_container.bin（容器格式）
    [string]$Bin = 'app.bin'
)

$ErrorActionPreference = 'Stop'

$Root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Bsp    = Join-Path $Root 'bsp'
$App    = Join-Path $Root 'app'
$Build  = Join-Path $Root 'build'
$ObjDir = Join-Path $Build 'obj'

# ---- 工具链 ----------------------------------------------------------------
# 只需要 RISC-V 工具链。**编译不需要 ESP-IDF**：本工程不 include 任何 IDF 头
# （唯一的外部头是编译器自带的 <stdint.h>/<stddef.h>）。
# python 只用来跑 tools/mkapp.py（打容器格式那个 bin）—— 纯标准库，随便哪个
# python 3 都行，这里顺手用 IDF 环境里那个。
$ToolDir = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf') -Directory |
           Sort-Object Name -Descending | Select-Object -First 1
$BinDir  = Join-Path $ToolDir.FullName 'riscv32-esp-elf\bin'
$Gcc     = Join-Path $BinDir 'riscv32-esp-elf-gcc.exe'
$Size    = Join-Path $BinDir 'riscv32-esp-elf-size.exe'
$Objcopy = Join-Path $BinDir 'riscv32-esp-elf-objcopy.exe'
$Objdump = Join-Path $BinDir 'riscv32-esp-elf-objdump.exe'
$PyExe   = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\python_env') -Directory -ErrorAction SilentlyContinue |
           Sort-Object Name -Descending |
           ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
           Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $PyExe) { $PyExe = 'python' }   # 退而求其次：PATH 里的 python

if ($Rebuild -and (Test-Path $ObjDir)) { Remove-Item -Recurse -Force $ObjDir }
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

$src = @()
$src += Get-ChildItem $Bsp -Include *.c,*.S -File -Recurse | Select-Object -ExpandProperty FullName
$src += Get-ChildItem $App -Include *.c -File -Recurse | Select-Object -ExpandProperty FullName

$Common = @(
    '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
    '-O2', '-g',
    '-ffreestanding', '-nostdlib', '-nostartfiles', '-fno-builtin',
    '-ffunction-sections', '-fdata-sections',
    '-Wall', '-Wextra', '-Wno-unused-parameter',
    '-Wl,--no-warn-rwx-segments', '-Wl,--gc-sections',
# bsp\\idf_headers = 冻结进来的 IDF 头（RMT 那几个，见 tools\\sync_idf_headers.ps1）
    "-I$Bsp", "-I$App", "-I$Bsp\idf_headers"
)

Write-Host "toolchain : $BinDir"
Write-Host "sources   : $($src.Count)"

$objs = @()
$failed = $false
foreach ($f in $src) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($f)
    $obj  = Join-Path $ObjDir ($name + '.o')
    $objs += $obj
    if ((Test-Path $obj) -and (-not $Rebuild)) {
        if ((Get-Item $obj).LastWriteTime -gt (Get-Item $f).LastWriteTime) { continue }
    }
    & $Gcc @Common @('-c', $f, '-o', $obj)
    if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED: $f" -ForegroundColor Red; $failed = $true }
}
if ($failed) { exit 1 }

Push-Location $Build
try {
    & $Gcc @Common "-Wl,-Map=$Build\app.map" '-T' (Join-Path $Bsp 'linker.ld') `
        '-o' (Join-Path $Build 'app.elf') @objs
    if ($LASTEXITCODE -ne 0) { Write-Host 'LINK FAILED' -ForegroundColor Red; exit 1 }
    Write-Host '=== built app.elf ==='
    & $Size app.elf

    # ★ **app.bin 就是纯 objcopy 扁平镜像** —— 直接 `objcopy -O binary`，
    #   不经过任何自定义脚本/段表，所以 **app.bin 直接拖进 S31-BOOT 盘就能用**。
    #   bootloader 会自动识别（开头不是 'S31A' 就当扁平镜像，整块装到
    #   S31_APP_FLAT_VMA = PSRAM 起点，入口也在那儿）。
    #   前提约定：① 链接脚本把 .boot 放最前面；② 镜像的 LMA 在 flash 装载区
    #   （0x40100000），见 bsp/linker.ld 与 boot_msc_s31/bsp/s31_layout.h。
    & $Objcopy '-O' 'binary' app.elf app.bin
    if ($LASTEXITCODE -ne 0) { Write-Host 'OBJCOPY FAILED' -ForegroundColor Red; exit 1 }

    # ★ **app_container.bin = 格式 A（带段表的容器）**，由 tools/mkapp.py 产出，
    #   而且是**用同一个 ELF 再做一次链接**得来的（装载基准往后挪 192 字节）。
    #
    #   为什么要第二次链接：
    #     容器格式的镜像前面有 192 字节的头+段表，payload 整体后移。
    #     而 App 的 startup.S 是"按文件内偏移 == flash 装载偏移"去读 .data 初值的
    #     （`la t0, __data_lma`），所以那种构建的 LMA 基准必须也是 0x40100000+192。
    #     不挪的话读到的正好是那张段表 —— 实测 App 能跑但 `.data magic` 读成 0x00000000。
    #     （挪了之后 payload 落点 = flash 0x1000C0，正好是 App 以为自己在的地方 ✓）
    #
    #   两种镜像的对照（真板都验过）：
    #     app.bin            扁平：bootloader 整块搬到 0x50000000；.data 初值顺带落在
    #                        0x50001xxx 没人用的地方，由 App 的 startup 从 flash 搬
    #     app_container.bin  容器：bootloader 按段表搬（.image→0x50000000、
    #                        .data/.sdata→0x50800000、.bss 清零）；App 再搬一遍等于多道保险
    #   ⇒ 日常拖 app.bin 就行；容器是留给"运行位置不止一处、扁平表达不了"的场合。
    $ldargs = @()
    $sects = $null
    & $Gcc @Common "-Wl,-Map=$Build\app_container.map" '-T' (Join-Path $Bsp 'linker.ld') `
        '-Wl,--defsym=S31_LMA_BASE=0x401000C0' `
        '-o' (Join-Path $Build 'app_container.elf') @objs
    if ($LASTEXITCODE -ne 0) { Write-Host 'LINK(container) FAILED' -ForegroundColor Red; exit 1 }

    & $PyExe (Join-Path $Root 'tools\mkapp.py') app_container.elf app_container.bin
    if ($LASTEXITCODE -ne 0) { Write-Host 'MKAPP FAILED' -ForegroundColor Red; exit 1 }

    # 守门 1：容器必须真的以 'S31A' 开头（否则 bootloader 会把它当扁平镜像装到
    #         0x50000000 —— 那是 64 字节头 + 段表，跑起来就是一堆垃圾指令）
    $magic = [System.IO.File]::ReadAllBytes((Join-Path $Build 'app_container.bin'))[0..3]
    if (-not ($magic[0] -eq 0x53 -and $magic[1] -eq 0x33 -and $magic[2] -eq 0x31 -and $magic[3] -eq 0x41)) {
        Write-Host '[x] app_container.bin 开头不是 S31A —— bootloader 会把它当扁平镜像，必飞' -ForegroundColor Red
        exit 1
    }
    # 守门 2：容器构建的装载基准必须正好是"扁平基准 + 头长"，
    #         否则 App 的 startup 会在 flash 里读错地方（上面那段注释讲的就是这个坑）
    $lmaContainer = (& $Objdump '-h' app_container.elf |
        Select-String -Pattern '^\s*\d+\s+\.image\s+\S+\s+\S+\s+([0-9a-f]+)' |
        Select-Object -First 1).Matches.Groups[1].Value
    if ([Convert]::ToInt64($lmaContainer, 16) -ne 0x401000C0) {
        Write-Host ("[x] 容器构建的 .image LMA = 0x{0}，应该是 0x401000C0" -f $lmaContainer) -ForegroundColor Red
        Write-Host '    说明 --defsym=S31_LMA_BASE=0x401000C0 没生效，或者 mkapp.py 的头长变了' -ForegroundColor Yellow
        exit 1
    }
    Write-Host ("=== app_container.bin : {0} bytes（带段表，也能拖）===" -f `
                (Get-Item app_container.bin).Length)

    # 守门 3：★ 容器去掉 192 字节头之后，payload 应该**正好等于扁平镜像**。
    #   这条不变量把"payload 必须保持 LMA 布局"钉死了 ——
    #   一旦 mkapp.py 又把区段紧挨着拼（不补段间对齐空隙），这里立刻报错。
    #   实测踩过：.image 与 .data 之间有 2 字节空隙，紧拼导致 App 读 .data 初值
    #   整体错 2 字节（0x1234abcd -> 0x00001234）。
    $flatSize = (Get-Item app.bin).Length
    $contSize = (Get-Item app_container.bin).Length - 192
    if ($contSize -ne $flatSize) {
        Write-Host ("[x] 容器 payload {0} B != 扁平镜像 {1} B —— payload 没保持 LMA 布局！" -f `
                    $contSize, $flatSize) -ForegroundColor Red
        Write-Host '    看 tools/mkapp.py 里"组装 payload"那段（段间空隙要补 0）' -ForegroundColor Yellow
        exit 1
    }
    Write-Host ("    （已核对：payload {0} B == 扁平镜像，说明 LMA 布局保住了）" -f $contSize)

    # ★★ 命门检查：扁平镜像的大小必须正好等于**各装载段 LMA 的总跨度**。
    #    这条检查是拿真事换来的：链接脚本一度按段分别写 `> PSRAM AT> FLASH`，
    #    于是 ld 给各段紧凑分配 LMA、**不镜像 VMA 侧的对齐空隙**（.clic_entry 的
    #    LMA 落在 0x00100082 而 VMA 在 0x500000c0，差 62 字节）。
    #    objcopy 是按 LMA 排布字节的，结果扁平镜像变成 4188 B、而期望跨度 4252 B
    #    —— 整块拷到基址后 .text 会整体错位，必崩。
    #    正确写法：镜像区里"偏移必须一一对应"的那些段放进**一个**输出段（见 linker.ld）。
    $sects = @()
    $lastSect = $null
    & $Objdump '-h' app.elf | ForEach-Object {
        if ($_ -match '^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)') {
            $lastSect = [pscustomobject]@{
                name = $Matches[1]
                size = [Convert]::ToInt64($Matches[2], 16)
                vma  = [Convert]::ToInt64($Matches[3], 16)
                lma  = [Convert]::ToInt64($Matches[4], 16)
                flags = ''
            }
            $sects += $lastSect
        } elseif ($lastSect -and $_ -match '(CONTENTS|ALLOC)') {
            $lastSect.flags += $_
        }
    }
    # 只算"既 ALLOC 又有 CONTENTS 且**非空**"的段 —— 那才是 objcopy 会写进 bin 的东西
    # （空段（size==0）的 LMA 是 ld 随手给的，不代表镜像内容，必须排除）
    $load = @($sects | Where-Object {
        $_.size -gt 0 -and $_.flags -match 'ALLOC' -and $_.flags -match 'CONTENTS' })
    $span = 0
    if ($load.Count -gt 0) {
        $lo = ($load | ForEach-Object { $_.lma } | Measure-Object -Minimum).Minimum
        $hi = ($load | ForEach-Object { $_.lma + $_.size } | Measure-Object -Maximum).Maximum
        $span = $hi - $lo
    }
    $flatSize = (Get-Item app.bin).Length
    Write-Host ("=== app.bin : {0} bytes（objcopy 直接产出，可直接拖）===" -f $flatSize)
    if ($span -ne 0 -and $flatSize -ne $span) {
        Write-Host ("[x] 扁平镜像 {0} B != 装载段 LMA 跨度 {1} B —— 文件偏移和装载偏移对不上！" -f `
                    $flatSize, $span) -ForegroundColor Red
        Write-Host '    多半是 linker.ld 里"偏移必须一一对应"的段没有放进同一个输出段' -ForegroundColor Yellow
        exit 1
    }
    Write-Host ("    （已核对：与装载段 LMA 跨度 {0} B 一致 -> 文件偏移 == 装载偏移）" -f $span)
}
finally { Pop-Location }

if ($Drag -or $DragFlat) {
    if ($Drive -eq '') {
        $vol = Get-Volume | Where-Object { $_.FileSystemLabel -eq 'S31-BOOT' } | Select-Object -First 1
        if (-not $vol) { Write-Host '没找到 S31-BOOT 盘（板子在 bootloader 模式吗？USB-HS 线插了吗？）' -ForegroundColor Red; exit 1 }
        $Drive = $vol.DriveLetter + ':'
    }
    $src = Join-Path $Build $Bin       # -Bin 可换：app_container.bin 就是容器格式那个
    if (-not (Test-Path $src)) { Write-Host "找不到 $src —— 先 make build" -ForegroundColor Red; exit 1 }
    Copy-Item $src (Join-Path $Drive $Bin) -Force
    Write-Host "已拖进 $Drive\$Bin（$((Get-Item $src).Length) 字节）—— 板子应在 1.5 秒后自动复位并跳进 App"
}
Write-Host 'done'
