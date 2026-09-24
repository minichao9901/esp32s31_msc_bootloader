#===========================================================================
# make.ps1 -- Makefile 背后真正干活的脚本（Makefile 只做薄封装）
#
#   make help        目标一览
#   make build       编译（增量）
#   make rebuild     全量重编
#   make clean       删 build\
#   make flash       编译 + 烧录到 flash 0x2000 + 读 2 秒日志
#   make monitor     一直读串口（Ctrl+C 退出）
#   make run         flash 然后 monitor
#   make erase       擦掉 App 描述符 -> 板子回到 bootloader 模式
#   make erase-all   整片擦除（**连 bootloader 一起擦**，之后要重新 make flash）
#
# 也可以直接调： pwsh -File tools\make.ps1 monitor -Port COM7
#===========================================================================

param(
    [Parameter(Position = 0)][string]$Action = 'help',
    [string]$Port = '',
    [int]$Seconds = 0
)

$ErrorActionPreference = 'Stop'

$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Tools = Join-Path $Root 'tools'
$Build = Join-Path $Root 'build'

# ---- 端口 / python --------------------------------------------------------
if ($Port -eq '') {
    $WsLocalEnv = Join-Path (Split-Path -Parent (Split-Path -Parent $Root)) 'local.env.ps1'
    if (Test-Path $WsLocalEnv) { . $WsLocalEnv }
    $Port = if ($env:ESP32_S31_PORT) { $env:ESP32_S31_PORT } else { 'COM43' }
}

function Get-Py {
    $p = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\python_env') -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending |
         ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
         Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $p) { throw "找不到 python（~\.espressif\python_env）—— esptool 要用它" }
    return $p
}

# ---- esptool 封装 ----------------------------------------------------------
# ⚠️ 全程 `--no-stub`（用 ROM 里的烧录程序，不下载 stub）。
#    代价是 **ROM 只认一部分命令** —— 实测 `erase-flash` 直接报：
#        ERROR: ESP32-S31 ROM does not support function erase_flash.
#    所以"整片擦除"要用 `erase-region 0x0 0x1000000` 代替（下面 erase-all 就是这么写的）。
function Invoke-Esptool {
    param([string[]]$EsptoolArgs)
    $py = Get-Py
    & $py -m esptool --chip esp32s31 -p $Port -b 460800 --no-stub @EsptoolArgs
    if ($LASTEXITCODE -ne 0) { throw "esptool 失败（exit $LASTEXITCODE）" }
}

# ---- 目标 ------------------------------------------------------------------
function Show-Help {
    Write-Host ""
    Write-Host "  boot_msc_s31 —— ESP32-S31 裸机 MSC 拖拽烧录 bootloader" -ForegroundColor Cyan
    Write-Host "  端口：$Port   （改法：`$env:ESP32_S31_PORT，或 make X PORT=COM7）" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  make build        编译（增量，快）"
    Write-Host "  make rebuild      全量重编"
    Write-Host "  make clean        删掉 build\ 目录"
    Write-Host "  make flash        编译 + 烧录到 flash 0x2000 + 读 2 秒日志"
    Write-Host "  make monitor      一直读串口（Ctrl+C 退出）"
    Write-Host "  make run          flash 然后 monitor"
    Write-Host "  make erase        擦掉 App 描述符（0x900000）-> 板子回到 bootloader 模式"
    Write-Host "  make erase-all    整片擦除 flash（连 bootloader 一起，之后要重新 make flash）"
    Write-Host ""
    Write-Host "  其它：" -ForegroundColor DarkGray
    Write-Host "    make size       看镜像大小" -ForegroundColor DarkGray
    Write-Host "    make headers    重新冻结 IDF 头到 bsp\idf_headers\（见 README §9）" -ForegroundColor DarkGray
    Write-Host ""
}

switch ($Action.ToLower()) {

    'help' {
        Show-Help
    }

    'build' {
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Tools 'build.ps1') -BuildOnly -Port $Port
        exit $LASTEXITCODE
    }

    'rebuild' {
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Tools 'build.ps1') -BuildOnly -Rebuild -Port $Port
        exit $LASTEXITCODE
    }

    'clean' {
        if (Test-Path $Build) {
            Remove-Item $Build -Recurse -Force
            Write-Host "已删除 $Build"
        } else {
            Write-Host "build\ 本来就不存在"
        }
    }

    'size' {
        $elf = Join-Path $Build 'app.elf'
        if (-not (Test-Path $elf)) { throw "还没有 build\app.elf —— 先 make build" }
        $size = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf') -Directory |
                Sort-Object Name -Descending | Select-Object -First 1 |
                ForEach-Object { Join-Path $_.FullName 'riscv32-esp-elf\bin\riscv32-esp-elf-size.exe' }
        & $size $elf
        Write-Host ("app.bin : {0} bytes" -f (Get-Item (Join-Path $Build 'app.bin')).Length)
    }

    'flash' {
        # build.ps1 自己会做"编译 -> elf2image -> 烧 0x2000 -> 读 2 秒日志"
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Tools 'build.ps1') -Port $Port -ReadSeconds 2
        exit $LASTEXITCODE
    }

    'monitor' {
        $py = Get-Py
        Write-Host "读 $Port（Ctrl+C 退出）..." -ForegroundColor Cyan
        & $py (Join-Path $Tools 'read_port.py') $Port $Seconds
        exit $LASTEXITCODE
    }

    'run' {
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Tools 'build.ps1') -Port $Port -ReadSeconds 1
        $py = Get-Py
        Write-Host ""
        Write-Host "=== 继续读串口（Ctrl+C 退出）===" -ForegroundColor Cyan
        & $py (Join-Path $Tools 'read_port.py') $Port 0
        exit $LASTEXITCODE
    }

    'erase' {
        # 只擦描述符那块（4KB）—— App 镜像还在 flash 里，但描述符没了，
        # 下次开机 app_probe() 会判"没有可跑的程序"，停在 bootloader 模式。
        Write-Host "擦除 App 描述符 @0x900000 ..." -ForegroundColor Yellow
        Invoke-Esptool @('erase-region', '0x900000', '0x1000')
        Write-Host "完成 —— 板子已复位，应该停在 bootloader 模式（会出现 S31-BOOT 盘）" -ForegroundColor Green
    }

    'erase-all' {
        # ⚠️ 这里**不能**用 `erase-flash`：--no-stub 下 S31 的 ROM 不认这个命令，
        #    会报 "ESP32-S31 ROM does not support function erase_flash"（实测踩过）。
        #    ROM 支持的等价写法是 erase-region 覆盖整片。
        Write-Host "整片擦除 flash（16MB，会连 bootloader 一起擦掉）..." -ForegroundColor Yellow
        Invoke-Esptool @('erase-region', '0x0', '0x1000000')
        Write-Host "完成 —— 现在 flash 是空的，必须 make flash 把 bootloader 烧回去" -ForegroundColor Green
    }

    'headers' {
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Tools 'sync_idf_headers.ps1')
        exit $LASTEXITCODE
    }

    default {
        Write-Host "不认识的目标：$Action" -ForegroundColor Red
        Show-Help
        exit 1
    }
}
