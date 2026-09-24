#!/usr/bin/env python3
"""读串口并原样打印（UTF-8 + errors=replace）。

用法: python read_port.py <PORT> [秒数]

    秒数省略 = 6 秒
    秒数 0 或负数 = **一直读**，Ctrl+C 退出（`make monitor` 就是这种）

注意：pyserial 打开端口默认 dtr=rts=True，会把板子按进下载模式 →
这里显式置 False（USB-Serial/JTAG 上这两个信号由外设转成复位/strapping 行为）。
⚠️ 所以**打开端口本身就会让芯片复位一次**，开机日志会从头来。
"""
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

port = sys.argv[1] if len(sys.argv) > 1 else "COM43"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
forever = secs <= 0

s = serial.Serial(port, 115200, timeout=0.2)
try:
    s.setDTR(False)
    s.setRTS(False)
except Exception:
    pass
# 注意：这里**不要** reset_input_buffer()。
# 打开端口会让芯片复位，开机日志紧接着就来，清输入缓冲会把最早那批字节丢掉。

if forever:
    sys.stdout.write("[read_port] 正在读 %s（Ctrl+C 退出）\n" % port)
    sys.stdout.flush()

t0 = time.time()
n = 0
try:
    while forever or (time.time() - t0 < secs):
        try:
            d = s.read(4096)
        except Exception as e:
            sys.stdout.write("\n[read error] %s\n" % e)
            break
        if d:
            n += len(d)
            sys.stdout.write(d.decode("utf-8", errors="replace"))
            sys.stdout.flush()
except KeyboardInterrupt:
    sys.stdout.write("\n[read_port] Ctrl+C\n")
finally:
    s.close()

sys.stdout.write("\n[read_port] %d bytes in %.1fs\n" % (n, time.time() - t0))
