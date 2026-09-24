#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkapp.py -- 把裸机 App 的 ELF 打成 bootloader 能拖拽烧录的 app.bin

⚠️ **这个工具现在默认不用了。** `s31_app` 走的是"经典模型"
   （LMA 在 flash、VMA 在 PSRAM），打出来的是**纯 `objcopy -O binary` 的扁平镜像**
   —— 那才是推荐格式（见 boot_msc_s31/README.md §3.1）。
   mkapp.py 产的是**格式 A（带段表的容器）**，它是为"**多区域 VMA**"准备的兼容路径：
   当 App 的运行位置不止一处（比如 .text 在 PSRAM、.data 在内部 RAM）时，
   扁平格式表达不了，只能用段表。

   🚨 **本工具只支持"LMA == 每个段各自的 VMA"那种老式布局。**
   现在 s31_app 的链接脚本把 LMA 放在 flash 映射窗口（0x40100000）里，
   bootloader 是**写不了那个地址**的（那是 flash 的只读别名）。
   所以如果你拿现在的 s31_app 跑 mkapp.py，它会直接报错退出（见下面 check_load_addrs）——
   这是故意的，免得打出一个"能拖进去但一跑就飞"的 bin。

镜像格式（见 boot_msc_s31/bsp/s31_layout.h）：
    64 字节头：
        magic 'S31A' / version=1 / entry / nsegs / total_size / crc32
    段表（每段 16 字节）：off / vma / size / flags
        off   相对镜像起点的偏移（bootloader 会去 flash 的 base+off 读）
        vma   目标虚拟地址（PSRAM 0x50000000 或内部 RAM [0x2F060000, 0x2F07AFC0)）
        flags bit0 = 1 表示"只清零不搬"（bss）
    crc32 覆盖 [0, total_size)，计算时 crc32 字段本身按 0 计

★ 为什么要按段打包（不能用 `objcopy -O binary` 一把梭）：
   万一 App 的 VMA 分在两处（PSRAM 放只读的 .text/.rodata，
   内部 RAM 放可写的 .data/.bss），`objcopy -O binary` 会**从最低 VMA 一直拍平到最高 VMA**
   —— 0x2F060000 和 0x50000000 之间隔着 ~550MB，拍出来就是一坨 550MB 的镜像。
   所以这里直接从 ELF 里**按段取内容**，按 VMA 连续性合并成若干"区段"，
   再拼头 + 段表 + CRC32。不依赖 objcopy。

用法:
    python mkapp.py app.elf app.bin            # 自动按 ELF 段布局打包
    python mkapp.py app.elf app.bin --dump     # 只打印段表，不写文件
"""
import argparse
import os
import struct
import subprocess
import sys

# 输出强制 UTF-8：Windows 上 Python 默认按控制台的 GBK 编码打印，
# 而这个工程的日志/输出一律 UTF-8（跟 read_port.py 一致），
# 不统一的话在别的终端里全是乱码。
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

MAGIC = 0x41313353          # 'S31A'
MAX_SEGS = 8
HDR_SIZE = 64
SEG_SIZE = 16
MAX_GAP = 64                # 合并区段时允许被零填充的最大空隙（对齐填充）


def find_tool(name):
    root = os.path.join(os.environ.get('USERPROFILE', ''), '.espressif', 'tools', 'riscv32-esp-elf')
    if os.path.isdir(root):
        for d in sorted(os.listdir(root), reverse=True):
            p = os.path.join(root, d, 'riscv32-esp-elf', 'bin', name + '.exe')
            if os.path.exists(p):
                return p
    return name


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        raise SystemExit('command failed: %s\n%s' % (' '.join(cmd), r.stderr[:400]))
    return r.stdout


def parse_sections(elf):
    """objdump -h -> [{'name','size','vma','lma','off','copy'}, ...]（按 VMA 排序）

    列布局：Idx Name Size VMA LMA FileOff Algn，下一行是 flags。
    """
    out = run([find_tool('riscv32-esp-elf-objdump'), '-h', elf])
    lines = out.splitlines()
    secs = []
    for i, ln in enumerate(lines):
        parts = ln.split()
        if len(parts) < 7 or not parts[0].isdigit():
            continue
        try:
            size = int(parts[2], 16)
            vma = int(parts[3], 16)
            lma = int(parts[4], 16)
            foff = int(parts[5], 16)
        except ValueError:
            continue
        name = parts[1]
        flags = lines[i + 1] if i + 1 < len(lines) else ''
        if size == 0 or 'ALLOC' not in flags:
            continue
        secs.append({
            'name': name, 'size': size, 'vma': vma, 'lma': lma, 'off': foff,
            'copy': 'CONTENTS' in flags,          # NOBITS（.bss/.sbss）= 只清零
        })
    secs.sort(key=lambda s: s['vma'])
    return secs


def group_runs(secs, want_copy, key):
    """把连续的段并成区段（空隙 ≤ MAX_GAP 的用 0 填）。

    ★ 归并用的 key：
      - 只清零的段（ZERO，.bss/.sbss）用 **'vma'** —— 它们没有"装载位置"，
        只有运行位置。
      - 有内容的段（COPY）**不要用这个函数**，用下面的 group_copy_runs() ——
        它还要看 VMA/LMA 的差值。

    返回值按 key 升序： [{'<key>','size','secs':[...]}]
    ⚠️ 空隙超过 MAX_GAP 就断开 —— 那条界线正是"镜像区 ↔ 别的区域"的分界，
       不能把几百 MB 的空洞填进来。
    """
    runs = []
    for s in secs:
        if s['copy'] != want_copy:
            continue
        if runs:
            cur = runs[-1]
            end = cur[key] + cur['size']
            if 0 <= (s[key] - end) <= MAX_GAP:
                cur['size'] = s[key] + s['size'] - cur[key]
                cur['secs'].append(s)
                continue
        runs.append({key: s[key], 'size': s['size'], 'secs': [s]})
    return runs


def group_copy_runs(secs):
    """有内容的段：按 **LMA** 归并成 payload 区段，但**落位地址用各自的 VMA**。

    为什么要多看一个"delta"（= vma - lma）：

        payload 在文件里是按 **LMA** 连续排的（objcopy 也是这么排的），
        但**落位目标**（VMA）不一定是同一块内存。经典模型下就是两种情况：

            .image   LMA 0x40100000 -> VMA 0x50000000    delta = 0x0FF00000
            .data    LMA 0x401013a0 -> VMA 0x50800000    delta = 0x0F6FEC60
            .sdata   LMA 0x401014a0 -> VMA 0x50800100    delta = 0x0F6FEC60

        .image 和 .data 的 LMA 是**紧挨着**的，可 VMA 差了 8MB ——
        一个 seg 表达不了（seg 只有一个 vma），所以**delta 一变就必须切段**。
        .data 和 .sdata 的 delta 相同、LMA 也连续，才能合成一个 seg。

    返回值： [{'lma','vma','size','delta','secs':[...]}]，按 LMA 升序。
    """
    runs = []
    for s in sorted(secs, key=lambda x: x['lma']):
        if not s['copy']:
            continue
        d = s['vma'] - s['lma']
        if runs:
            cur = runs[-1]
            end = cur['lma'] + cur['size']
            if cur['delta'] == d and 0 <= (s['lma'] - end) <= MAX_GAP:
                cur['size'] = s['lma'] + s['size'] - cur['lma']
                cur['secs'].append(s)
                continue
        runs.append({'lma': s['lma'], 'vma': s['vma'], 'delta': d,
                     'size': s['size'], 'secs': [s]})
    return runs


def crc32(data, crc=0):
    crc = crc ^ 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1) & 0xFFFFFFFF))
    return crc ^ 0xFFFFFFFF


def sym(elf, name):
    out = run([find_tool('riscv32-esp-elf-nm'), elf])
    for ln in out.splitlines():
        parts = ln.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def check_load_addrs(copy_runs, zero_runs):
    """守住一个很容易踩的坑：bootloader 只会把内容写到 **可写内存**。

    容器格式的语义是"bootloader 把第 i 段放到 seg[i].vma"，所以每个 seg 的
    落位地址都必须落在可写内存里：

        PSRAM        0x50000000 ~ 0x54000000
        内部 RAM     0x2F060000 ~ 0x2F07AFC0

    🚨 反面教材：如果链接脚本的 **LMA 落在 flash 映射窗口 0x40100000**，
    而这里又错误地把 LMA 当成落位地址（早期版本就是这么写的），
    bootloader 就会往 flash 的**只读别名**上写 -> 直接 store access fault，
    现象是"能拖进去、一跑就飞"。所以这道检查必须留着。
    """
    def writable(a):
        return (0x50000000 <= a < 0x54000000) or (0x2F060000 <= a < 0x2F07AFC0)

    bad = [('COPY', r['vma']) for r in copy_runs if not writable(r['vma'])]
    bad += [('ZERO', r['vma']) for r in zero_runs if not writable(r['vma'])]
    if bad:
        which = ', '.join('%s@0x%08x' % (w, a) for w, a in bad[:4])
        raise SystemExit(
            '这些区段的落位地址不在可写内存里：%s\n'
            '  -> 检查 linker.ld：段落的 VMA 必须是 PSRAM 或 App 可用的内部 RAM，\n'
            '     不能是 flash 映射窗口（0x40000000~0x50000000，那是只读别名）。'
            % which)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('out', nargs='?', default=None)
    ap.add_argument('--dump', action='store_true', help='只打印段表，不写文件')
    ap.add_argument('--single', action='store_true', help='(已废弃，保留兼容)')
    args = ap.parse_args()

    entry = sym(args.elf, '_start')
    if entry is None:
        raise SystemExit('拿不到 _start')

    secs = parse_sections(args.elf)
    if not secs:
        raise SystemExit('ELF 里没找到任何 ALLOC 段')

    with open(args.elf, 'rb') as f:
        elfdata = f.read()

    copy_runs = group_copy_runs(secs)            # 有内容的：按 LMA 归并，落位用 VMA
    zero_runs = group_runs(secs, False, 'vma')   # .bss/.sbss：按运行地址归并

    # ★ 落位地址必须在可写内存里（防"往 flash 只读别名上写"那种蠢事）
    check_load_addrs(copy_runs, zero_runs)

    # ---- 组装 payload ----
    # ★ 关键：payload 里**必须保持 LMA 布局**（段间的小对齐空隙要补 0），
    #   不能把各区段紧挨着拼起来！因为 App 的 startup.S 是
    #   "按文件内偏移 == flash 装载偏移" 去读 .data 初值的（`la t0, __data_lma`）。
    #   实测踩过：`.image` 和 `.data` 的 LMA 差 2 字节对齐空隙，
    #   紧拼的话 .data 初值在文件里前移 2 字节 -> App 读出来整体错 2 字节
    #   （0x1234abcd 读成 0x00001234）。
    #   ⚠️ 空隙**大于** MAX_GAP 时只能紧凑排（那种情况属于"多区域 VMA"，
    #      本来就不该依赖 LMA 读取，全靠段表搬运）。
    payload = bytearray()
    segs = []
    base_lma = None
    tight = 0
    for r in copy_runs:
        if base_lma is None:
            base_lma = r['lma']
            off = 0
        else:
            want = r['lma'] - base_lma          # 它按 LMA 布局"应该"在 payload 的哪个位置
            gap = want - len(payload)
            if 0 <= gap <= MAX_GAP:
                payload += bytearray(gap)       # 补上对齐空隙，保持 LMA 布局
                off = len(payload)
            else:
                off = len(payload)              # 离太远：紧凑排
                tight += 1
        buf = bytearray(r['size'])              # 预置 0，用来填段内的空隙
        for s in r['secs']:
            rel = s['lma'] - r['lma']
            buf[rel:rel + s['size']] = elfdata[s['off']:s['off'] + s['size']]
        payload += buf
        # ★ 落位用 **VMA**：bootloader 把这段直接放到它该在的地方。
        #   （App 的 startup 之后还会从 LMA 自己搬一遍 .data —— 内容一样，
        #     等于多一道保险，无害。）
        segs.append((off, r['vma'], r['size'], 0))

    if tight:
        print('  ⚠ %d 个区段的 LMA 离得太远（> %d B），已改为紧凑排布；'
              '这种镜像下 App **不能**依赖 __data_lma 从 flash 读初值，'
              '只能靠段表搬运。' % (tight, MAX_GAP))

    for r in zero_runs:
        segs.append((0, r['vma'], r['size'], 1))

    if len(segs) > MAX_SEGS:
        raise SystemExit('段数 %d 超过上限 %d' % (len(segs), MAX_SEGS))

    # ---- 头 + 段表（off 相对镜像起点）----
    hdr_len = HDR_SIZE + SEG_SIZE * MAX_SEGS
    img = bytearray(hdr_len)
    struct.pack_into('<IIIIII', img, 0, MAGIC, 1, entry, len(segs), 0, 0)
    for i, (off, vma, size, flags) in enumerate(segs):
        struct.pack_into('<IIII', img, HDR_SIZE + SEG_SIZE * i,
                         off + hdr_len, vma, size, flags)

    body = bytearray(img) + payload
    struct.pack_into('<I', body, 16, len(body))
    struct.pack_into('<I', body, 20, 0)
    c = crc32(bytes(body))
    struct.pack_into('<I', body, 20, c)

    print('=== %s ===' % (args.out or args.elf))
    print('  entry      : 0x%08x' % entry)
    print('  image total: %d B (段表头 %d + payload %d)'
          % (len(body), hdr_len, len(payload)))
    print('  crc32      : 0x%08x' % c)
    print('  segments   :')
    for i, (off, vma, size, flags) in enumerate(segs):
        kind = 'ZERO' if flags & 1 else 'COPY'
        if not (flags & 1):
            names = ' <- ' + '+'.join(s['name'] for s in copy_runs[i]['secs'])
        else:
            names = ' <- ' + '+'.join(s['name'] for s in zero_runs[i - len(copy_runs)]['secs'])
        region = 'PSRAM' if 0x50000000 <= vma < 0x54000000 else 'IRAM'
        what = '清零' if (flags & 1) else '装到'
        print('    [%d] %-4s %-5s -> 0x%08x size=%6d off=%6d (%s%s)%s'
              % (i, kind, region, vma, size, off, what, 'VMA', names))

    if args.dump or not args.out:
        return

    with open(args.out, 'wb') as f:
        f.write(bytes(body))
    print('  -> %s' % args.out)


if __name__ == '__main__':
    main()
