#===========================================================================
# build.ps1 -- 构建 / 烧录 / 读日志：ESP32-S31 裸机 MSC bootloader
#
#   pwsh -File tools\build.ps1                # 编译 + 烧录 + 读日志
#   pwsh -File tools\build.ps1 -BuildOnly     # 只编译
#   pwsh -File tools\build.ps1 -Rebuild       # 全量重编
#   pwsh -File tools\build.ps1 -ReadSeconds 8
#
# 全程不碰 idf.py / CMake / IDF 环境：
#   编译器  riscv32-esp-elf-gcc（从 ~\.espressif\tools 找）
#   烧录    python -m esptool（独立 esptool，原生支持 esp32s31）
#   镜像烧到 flash 0x2000，由芯片 bootROM 直接加载（不用 IDF bootloader）
#===========================================================================

param(
    [switch]$BuildOnly,
    [switch]$Rebuild,
    [string]$Port = '',
    [int]$ReadSeconds = 6
)

$ErrorActionPreference = 'Stop'

$Root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Bsp    = Join-Path $Root 'bsp'
$App    = Join-Path $Root 'app'
$Cu     = Join-Path $Root 'cherryusb'
$Build  = Join-Path $Root 'build'
$ObjDir = Join-Path $Build 'obj'

# ---- 工具链（绝对路径，照 rtt_nano_s31 的做法）-----------------------------
$ToolDir = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf') -Directory |
           Sort-Object Name -Descending | Select-Object -First 1
$BinDir  = Join-Path $ToolDir.FullName 'riscv32-esp-elf\bin'
$Gcc     = Join-Path $BinDir 'riscv32-esp-elf-gcc.exe'
$Size    = Join-Path $BinDir 'riscv32-esp-elf-size.exe'

$PyExe = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\python_env') -Directory |
         Sort-Object Name -Descending |
         ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
         Where-Object { Test-Path $_ } | Select-Object -First 1

if ($Port -eq '') {
    $WsLocalEnv = Join-Path (Split-Path -Parent (Split-Path -Parent $Root)) 'local.env.ps1'
    if (Test-Path $WsLocalEnv) { . $WsLocalEnv }
    $Port = if ($env:ESP32_S31_PORT) { $env:ESP32_S31_PORT } else { 'COM43' }
}

if ($Rebuild -and (Test-Path $ObjDir)) { Remove-Item -Recurse -Force $ObjDir }
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

# ---- 源文件 ----------------------------------------------------------------
$src = @()
$src += Get-ChildItem $Bsp -Include *.c,*.S -File -Recurse | Select-Object -ExpandProperty FullName
$src += Get-ChildItem $App -Include *.c -File -Recurse | Select-Object -ExpandProperty FullName
if (Test-Path $Cu) {
    $src += Join-Path $Cu 'core\usbd_core.c'
    $src += Join-Path $Cu 'class\msc\usbd_msc.c'
    $src += Join-Path $Cu 'port\dwc2\usb_dc_dwc2.c'
    $src += Join-Path $Cu 'osal\usb_osal_bare.c'
    $src = $src | Where-Object { Test-Path $_ }
}

$inc = @(
    "-I$Bsp",
    "-I$App",
    "-I$Root"
)

# ---- IDF 头文件（**已经冻结进工程**，不再依赖 IDF 源码树）-------------------
# bsp\idf_headers\ 里是 bsp/s31_psram.c 用到的那几个 IDF LL 头 + 它们的传递闭包
# （由 tools\sync_idf_headers.ps1 用 `gcc -M` **问编译器**生成的，不会漏）。
#
# 为什么非要 IDF 的头：PSRAM 那块**按字段名写寄存器**，不手抄位号 ——
# 本项目因为手抄位号栽过两次（MSPI_DIV.FB_DIV 实际在 bit[7:3]、
# AP PSRAM 的 MR0/MR4/MR8 位域全拍错），各花掉一整轮。见 PSRAM.md §2。
#
# 现在整个工程 **100% 自包含**：换台机器只要有 RISC-V 工具链就能编，不用装 IDF。
# 要重新同步这些头：pwsh -File tools\sync_idf_headers.ps1
$IdfHdrs = Join-Path $Bsp 'idf_headers'
if (-not (Test-Path -LiteralPath $IdfHdrs)) {
    Write-Host ""
    Write-Host "找不到冻结的 IDF 头目录：$IdfHdrs" -ForegroundColor Red
    Write-Host "本工程靠它提供 4 个 IDF LL 头（详见本文件上面的注释）。" -ForegroundColor Yellow
    Write-Host "重新生成：pwsh -File tools\sync_idf_headers.ps1 -IdfPath <你的 esp-idf 路径>" -ForegroundColor Yellow
    exit 1
}
# ⚠️ 放在**最后**：让工程自己的同名文件优先命中
$inc += @("-I$IdfHdrs")
if (Test-Path $Cu) {
    $inc += @("-I$Cu\core", "-I$Cu\common", "-I$Cu\class\msc",
              "-I$Cu\port\dwc2", "-I$Cu\osal", "-I$Cu\include")
}

# -nostdlib：不链 newlib（bsp/mini_libc.c 自己给 memcpy/printf/64 位除法）
# -mabi=ilp32f 但不开 FPU（startup.S 里 FS=0）→ 一旦误用浮点会立刻异常，不会静默出错
$Common = @(
    '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
    '-O2', '-g',
    '-ffreestanding', '-nostdlib', '-nostartfiles', '-fno-builtin',
    '-ffunction-sections', '-fdata-sections',
    '-Wall', '-Wextra', '-Wno-unused-parameter',
    # ★ 把源码的**绝对路径**从产物里抹掉：CherryUSB 有几处把 __FILE__ 编进字符串
    #   （assert/日志），不映射的话 app.bin 会随"工程放在哪个目录"而变，
    #   换个路径编译出来的镜像就不是同一份了（实测差 84 字节）。
    #   映射成相对路径之后：产物可复现、也不会把本机路径带出去。
    "-ffile-prefix-map=$Root=.",
    '-Wl,--no-warn-rwx-segments', '-Wl,--gc-sections'
) + $inc

Write-Host "toolchain : $BinDir"
Write-Host "sources   : $($src.Count)"

# ---- 逐文件编译 ------------------------------------------------------------
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
    if ($LASTEXITCODE -ne 0) {
        Write-Host "COMPILE FAILED: $f" -ForegroundColor Red
        $failed = $true
    }
}
if ($failed) { exit 1 }

# ---- 链接 ------------------------------------------------------------------
Push-Location $Build
try {
    & $Gcc @Common "-Wl,-Map=$Build\app.map" '-T' (Join-Path $Bsp 'linker.ld') `
        '-o' (Join-Path $Build 'app.elf') @objs
    if ($LASTEXITCODE -ne 0) { Write-Host 'LINK FAILED' -ForegroundColor Red; exit 1 }
    Write-Host '=== built app.elf ==='
    & $Size app.elf

    & $PyExe -m esptool --chip esp32s31 elf2image -fm dio -ff 80m -fs 16MB -o app.bin app.elf
    if ($LASTEXITCODE -ne 0) { Write-Host 'ELF2IMAGE FAILED' -ForegroundColor Red; exit 1 }
    Write-Host ("=== app.bin : {0} bytes ===" -f (Get-Item app.bin).Length)
}
finally { Pop-Location }

if ($BuildOnly) { Write-Host 'done (no flash)'; exit 0 }

# ---- 烧录到 0x2000 ---------------------------------------------------------
Push-Location $Build
try {
    & $PyExe -m esptool --chip esp32s31 -p $Port -b 460800 --no-stub `
        --before default-reset --after hard-reset `
        write-flash -fm dio -ff 80m -fs 16MB 0x2000 app.bin
    if ($LASTEXITCODE -ne 0) { Write-Host 'FLASH FAILED' -ForegroundColor Red; exit 1 }
}
finally { Pop-Location }

# ---- 读日志 ----------------------------------------------------------------
Start-Sleep -Milliseconds 800
& $PyExe (Join-Path $Root 'tools\read_port.py') $Port $ReadSeconds
Write-Host 'done'

