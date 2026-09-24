# ============================================================
#  s31_app —— ESP32-S31 裸机 App 模板（拖进 S31-BOOT 盘就能跑）
#
#    make help        目标一览（默认目标）
#    make build       编译 -> build\app.bin + build\app_container.bin
#    make flash       擦描述符 -> 等盘 -> 拷进去（全自动"烧录"）
#    make monitor     看串口
#
#  说明：
#    - **App 的"烧录"就是拖拽**：bootloader 模式下会出现 S31-BOOT 盘，
#      把 build\app.bin 拷进去即可。make flash / make drag 就是在做这件事。
#    - build\ 里有两个镜像（make build 一次产两个，都能拖）：
#        app.bin            扁平 objcopy 产物（默认拖这个）
#        app_container.bin  格式 A 带段表，bootloader 按段表逐个搬
#      想拖容器那个： make drag BIN=app_container.bin
#    - 逻辑都在 tools\*.ps1，这里只做薄封装（跟工作区根 Makefile 一个套路）
#    - 本机没有 sh.exe，显式用 cmd.exe 当 SHELL，必须配 /c
#    - 端口默认取 local.env.ps1 里的 ESP32_S31_PORT（本机 COM43 = USB-DBG）
#      临时换： make monitor PORT=COM7
#    - **不需要 ESP-IDF**：RMT 要用的那几个 IDF 头已冻结在 bsp\idf_headers\
#      （`make headers` 重新冻结，只在 IDF 改了 S31 寄存器定义时才需要跑）
# ============================================================

SHELL       := cmd.exe
.SHELLFLAGS := /c

PORT ?=
# 读串口读多少秒；0 = 一直读（Ctrl+C 退出）。想只看一段：make monitor SECONDS=8
SECONDS ?= 0
# 拖哪个镜像：app.bin（扁平，默认）/ app_container.bin（格式 A 带段表）
BIN ?= app.bin

PWSH := pwsh -NoProfile -ExecutionPolicy Bypass -File
MK   := $(PWSH) tools/make.ps1

.DEFAULT_GOAL := help

.PHONY: help build rebuild clean size headers drag flash monitor run erase

help:
	@$(MK) help

build:
	@$(MK) build -Port "$(PORT)"

rebuild:
	@$(MK) rebuild -Port "$(PORT)"

clean:
	@$(MK) clean

size:
	@$(MK) size

# 重新冻结 bsp\idf_headers\（改 IDF 那边 S31 寄存器定义时才要跑；跑完要重编）
headers:
	@$(PWSH) tools/sync_idf_headers.ps1

# 把 build\$(BIN) 拷进 S31-BOOT 盘（要求板子已经在 bootloader 模式）
drag:
	@$(MK) drag -Port "$(PORT)" -Bin "$(BIN)"

# 全自动：不在 bootloader 模式就先擦描述符，等盘出现再拷
flash:
	@$(MK) flash -Port "$(PORT)" -Bin "$(BIN)"

monitor:
	@$(MK) monitor -Port "$(PORT)" -Seconds $(SECONDS)

run:
	@$(MK) run -Port "$(PORT)" -Bin "$(BIN)"

erase:
	@$(MK) erase -Port "$(PORT)"
