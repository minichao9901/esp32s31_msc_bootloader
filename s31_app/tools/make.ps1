#===========================================================================
# make.ps1 -- Makefile 背后真正干活的脚本（Makefile 只做薄封装）
#
#   make help        目标一览
#   make build       编译（增量）-> build\app.bin
#   make rebuild     全量重编
#   make clean       删 build\
#   make size        看一眼段大小
#   make drag        编译 + 把 app.bin 拷进 S31-BOOT 盘（板子要已在 bootloader 模式）
#   make flash       擦描述符 -> 等 S31-BOOT 盘出现 -> 编译 + 拷进去（全自动）
#   make monitor     一直读串口（Ctrl+C 退出）
#   make run         flash 然后 monitor
#   make erase       擦掉 App 描述符 -> 板子回到 bootloader 模式
#   make headers     重新冻结 bsp\idf_headers\（只在 IDF 改了 S31 寄存器定义时才要跑）
#
# 也可以直接调： pwsh -File tools\make.ps1 drag -Port COM7
#
# 说明：**编译和"拖拽"这两件事都复用 tools\build.ps1**（它是唯一知道
#       编译参数和"往哪儿拷"的地方），这里只补它没有的东西：
#       找盘/等盘、esptool 擦除、读串口。
#===========================================================================

param(
    [Parameter(Position = 0)][string]$Action = 'help',
    [string]$Port = '',
    [int]$Seconds = 0,
    [int]$WaitVolumeSec = 25,
    # 拖哪个 bin：app.bin（扁平，默认）/ app_container.bin（容器格式）
    [string]$Bin = 'app.bin'
)

$ErrorActionPreference = 'Stop'

$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Tools = Join-Path $Root 'tools'
$Build = Join-Path $Root 'build'
# ⚠️ 这个变量**不能**叫 $Bin —— param 里已经有个 $Bin（拖哪个镜像），会互相覆盖
$FlatBin = Join-Path $Build 'app.bin'
$PS    = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File')

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
    if (-not $p) { throw '找不到 python（~\.espressif\python_env）—— esptool 要用它' }
    return $p
}

function Invoke-Esptool {
    param([string[]]$EsptoolArgs)
    $py = Get-Py
    & $py -m esptool --chip esp32s31 -p $Port -b 460800 --no-stub @EsptoolArgs
    if ($LASTEXITCODE -ne 0) { throw "esptool 失败（exit $LASTEXITCODE）" }
}

# ---- S31-BOOT 盘 ----------------------------------------------------------
# ⚠️ 每次复位后 Windows 可能重新分配盘符，所以**永远按卷标找**，别记盘符。
function Find-BootVolume {
    return Get-Volume -ErrorAction SilentlyContinue |
           Where-Object { $_.FileSystemLabel -eq 'S31-BOOT' } | Select-Object -First 1
}

function Wait-BootVolume {
    param([int]$TimeoutSec = 25)
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        $v = Find-BootVolume
        if ($v) { return $v }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# 编译 + 拖拽：完全交给 build.ps1（它才知道编译参数和盘符规则）
# ⚠️ 调用点**别**写成 `exit (Invoke-BuildScript)` —— 那样把函数返回值当 exit 参数时，
#    原生进程（pwsh/gcc）的输出会被整段吞掉，实测 `make build` 一声不响。
#    改成"函数只设脚本级退出码，调用点再 exit"就没这个问题。
$script:Rc = 0
function Invoke-BuildScript {
    param([switch]$Drag, [switch]$Rebuild)
    $a = @()
    if ($Rebuild) { $a += '-Rebuild' }
    if ($Drag)    { $a += '-Drag'; $a += @('-Bin', $Bin) }
    & pwsh @PS (Join-Path $Tools 'build.ps1') @a
    $script:Rc = $LASTEXITCODE
}

# ---- 目标 ------------------------------------------------------------------
function Show-Help {
    Write-Host ''
    Write-Host '  s31_app —— ESP32-S31 裸机 App 模板（拖进 S31-BOOT 盘就能跑）' -ForegroundColor Cyan
    Write-Host "  端口：$Port   （改法：`$env:ESP32_S31_PORT，或 make X PORT=COM7）" -ForegroundColor DarkGray
    Write-Host ''
    Write-Host '  make build        编译 -> build\app.bin + build\app_container.bin（**两个都产**）'
    Write-Host '  make rebuild      全量重编'
    Write-Host '  make clean        删掉 build\ 目录'
    Write-Host '  make size         看一眼各段大小'
    Write-Host '  make drag         编译 + 把 app.bin 拷进 S31-BOOT 盘（板子要已在 bootloader 模式）'
    Write-Host '  make flash        擦描述符 -> 等 S31-BOOT 盘出现 -> 编译 + 拷进去（全自动）'
    Write-Host '  make monitor      一直读串口（Ctrl+C 退出）'
    Write-Host '  make run          flash 然后 monitor'
    Write-Host '  make erase        擦掉 App 描述符（0x900000）-> 板子回到 bootloader 模式'
    Write-Host '  make headers      重新冻结 bsp\idf_headers\（IDF 改了 S31 寄存器定义时才要跑）'
    Write-Host ''
    Write-Host '  ★ App 的「烧录」就是**拖拽**：bootloader 模式下会出现 S31-BOOT 盘，' -ForegroundColor DarkGray
    Write-Host '    把 app.bin 拷进去即可（1.5 秒后自动复位并跳进 App）。' -ForegroundColor DarkGray
    Write-Host '    make flash 把"回 bootloader + 等盘 + 拷贝"这一串自动化了。' -ForegroundColor DarkGray
    Write-Host ''
    Write-Host '  build\ 里有两个镜像（make build 一起产）：' -ForegroundColor DarkGray
    Write-Host '    app.bin            扁平 objcopy 产物（默认拖这个）' -ForegroundColor DarkGray
    Write-Host '    app_container.bin  格式 A 带段表（bootloader 按段表逐个搬）' -ForegroundColor DarkGray
    Write-Host '    想拖容器那个： make drag BIN=app_container.bin' -ForegroundColor DarkGray
    Write-Host ''
}

switch ($Action.ToLower()) {

    'help' { Show-Help }

    'build' {
        Invoke-BuildScript; exit $script:Rc
    }

    'rebuild' {
        Invoke-BuildScript -Rebuild; exit $script:Rc
    }

    'clean' {
        if (Test-Path $Build) {
            Remove-Item $Build -Recurse -Force
            Write-Host "已删除 $Build"
        } else {
            Write-Host 'build\ 本来就不存在'
        }
    }

    'size' {
        $elf = Join-Path $Build 'app.elf'
        if (-not (Test-Path $elf)) { throw '还没有 build\app.elf —— 先 make build' }
        $size = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf') -Directory |
                Sort-Object Name -Descending | Select-Object -First 1 |
                ForEach-Object { Join-Path $_.FullName 'riscv32-esp-elf\bin\riscv32-esp-elf-size.exe' }
        & $size $elf
        Write-Host ('app.bin            : {0} bytes' -f (Get-Item $FlatBin).Length)
        $cb = Join-Path $Build 'app_container.bin'
        if (Test-Path $cb) {
            Write-Host ('app_container.bin  : {0} bytes' -f (Get-Item $cb).Length)
        }
    }

    'drag' {
        Invoke-BuildScript -Drag; exit $script:Rc
    }

    'flash' {
        # 全自动：不在 bootloader 模式就先擦描述符，等盘出现，再编译 + 拷贝
        if (-not (Find-BootVolume)) {
            Write-Host '板子现在不在 bootloader 模式 -> 擦掉描述符让它回去' -ForegroundColor Yellow
            Invoke-Esptool @('erase-region', '0x900000', '0x1000')
            Write-Host "等 S31-BOOT 盘出现（最多 $WaitVolumeSec 秒）..." -ForegroundColor DarkGray
            if (-not (Wait-BootVolume -TimeoutSec $WaitVolumeSec)) {
                Write-Host '等了很久还是没等到 S31-BOOT 盘。' -ForegroundColor Red
                Write-Host '检查：① USB-HS 线插了吗 ② 板子是不是卡住了（试着按住 GPIO0 上电）' -ForegroundColor Yellow
                exit 1
            }
            Write-Host '盘出现了' -ForegroundColor Green
        }
        Invoke-BuildScript -Drag; exit $script:Rc
    }

    'monitor' {
        $py = Get-Py
        Write-Host "读 $Port（Ctrl+C 退出）..." -ForegroundColor Cyan
        & $py (Join-Path $Tools 'read_port.py') $Port $Seconds
        exit $LASTEXITCODE
    }

    'run' {
        & pwsh @PS (Join-Path $Tools 'make.ps1') flash -Port $Port -Bin $Bin
        Start-Sleep -Seconds 3
        $py = Get-Py
        Write-Host ''
        Write-Host '=== 读串口（Ctrl+C 退出）===' -ForegroundColor Cyan
        & $py (Join-Path $Tools 'read_port.py') $Port 0
        exit $LASTEXITCODE
    }

    'erase' {
        Write-Host '擦除 App 描述符 @0x900000 ...' -ForegroundColor Yellow
        Invoke-Esptool @('erase-region', '0x900000', '0x1000')
        Write-Host '完成 —— 板子已复位，应该停在 bootloader 模式（会出现 S31-BOOT 盘）' -ForegroundColor Green
    }

    default {
        Write-Host "不认识的目标：$Action" -ForegroundColor Red
        Show-Help
        exit 1
    }
}
