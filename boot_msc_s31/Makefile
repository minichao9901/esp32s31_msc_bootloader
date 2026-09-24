# ============================================================
#  boot_msc_s31 —— ESP32-S31 裸机 CherryUSB MSC 拖拽烧录 bootloader
#
#    make help        目标一览（默认目标）
#    make build       编译
#    make flash       编译 + 烧录
#    make monitor     看串口
#    make erase       擦掉 App 描述符 -> 回到 bootloader 模式
#    make erase-all   整片擦除
#
#  说明：
#    - 逻辑都在 tools\*.ps1，这里只做薄封装（跟工作区根 Makefile 一个套路）
#    - 本机没有 sh.exe，显式用 cmd.exe 当 SHELL，必须配 /c
#    - 端口默认取 local.env.ps1 里的 ESP32_S31_PORT（本机 COM43 = USB-DBG）
#      临时换： make monitor PORT=COM7
#    - **不需要 ESP-IDF**：IDF 头已冻结在 bsp\idf_headers\（见 README §9）
# ============================================================

SHELL       := cmd.exe
.SHELLFLAGS := /c

PORT ?=
# 读串口读多少秒；0 = 一直读（Ctrl+C 退出）。想只看一段：make monitor SECONDS=8
SECONDS ?= 0

PWSH := pwsh -NoProfile -ExecutionPolicy Bypass -File
MK   := $(PWSH) tools/make.ps1

.DEFAULT_GOAL := help

.PHONY: help build rebuild clean size flash monitor run erase erase-all headers

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

flash:
	@$(MK) flash -Port "$(PORT)"

monitor:
	@$(MK) monitor -Port "$(PORT)" -Seconds $(SECONDS)

run:
	@$(MK) run -Port "$(PORT)"

erase:
	@$(MK) erase -Port "$(PORT)"

erase-all:
	@$(MK) erase-all -Port "$(PORT)"

# 重新从 IDF 源码树冻结那几个 LL 头（只有 S31 寄存器定义被修正时才需要）
headers:
	@$(MK) headers
