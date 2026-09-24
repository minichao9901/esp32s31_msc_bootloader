#!/usr/bin/env python3
"""把 ESP-IDF 某个源文件"按 IDF 自己的编译命令"重新编译成 .s / .i / .d，用来读真实展开结果。

为什么需要它：IDF 的 LL 层全是 `static inline` + 宏。看 `.i`（只做预处理）**看不到**
`psram_ctrlr_ll_xxx()` 到底写了哪个寄存器 —— 它们要到代码生成阶段才被 inline 展开。
所以要读"IDF 究竟按什么顺序、往哪个地址写什么值"，**`.s`（汇编）才是答案**；
`.i` 适合确认宏取到了什么值（例如 CONFIG_* 有没有生效）。

用法（在本工程目录下）：
    python tools/idf_compile_intel.py -B E:\\esp-idf-s31\\projects\\led_rgb\\build -m esp_psram_impl_ap_oct -k s
    python tools/idf_compile_intel.py -B <build> -m <源文件名正则> -k i
    python tools/idf_compile_intel.py -B <build> -l            # 列出所有可编译的源文件

-k s : 生成 .s（汇编，含寄存器地址注释）
-k i : 生成 .i（预处理结果，确认宏展开）
-k l : 反汇编已有的 .obj（不需要重新编译）—— 暂未实现，用 objdump 即可

输出默认写到 <build>/intel/<basename>.<ext>。
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def load(build_dir):
    path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(path):
        sys.exit("找不到 %s（先 idf.py build 一次）" % path)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def rewrite(cmd, out_path, kind):
    """把编译命令行改成"只生成 out_path"的形式。

    不做分词！IDF 的 -D 里带 \\" 转义引号（如 -DIDF_VER=\\"v6.1-dirty\\"），
    自己 split 会把引号切错、后面的参数全部错位（症状：stdlib.h 报 unknown type name '_mbstate_t'）。
    这里只在**原始字符串**上做定点替换，剩下的原样交给 cmd.exe —— 和 CMake 当年的语义完全一致。
    """
    # 去掉 "-o <obj>"（路径可能带引号）和单独的 "-c"
    cmd = re.sub(r'\s-o\s+(?:"[^"]*"|\S+)', ' ', cmd)
    cmd = re.sub(r'(^|\s)-c(\s|$)', r'\1', cmd)

    if kind == "s":
        extra = '-S -fverbose-asm -g0 -o "%s"' % out_path
    else:
        extra = '-E -P -o "%s"' % out_path
    # IDF 一般带 -Werror；我们只是"重放编译看展开结果"，不想被警告拦住
    return cmd + " " + extra + " -Wno-error -w"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-B", "--build", required=True, help="IDF 工程的 build 目录")
    ap.add_argument("-m", "--match", help="源文件路径的正则（如 esp_psram_impl_ap_oct）")
    ap.add_argument("-k", "--kind", default="s", choices=["s", "i"], help="s=汇编 i=预处理")
    ap.add_argument("-o", "--out", help="输出文件（缺省 <build>/intel/<name>.<kind>）")
    ap.add_argument("-l", "--list", action="store_true", help="只列出匹配到的源文件")
    a = ap.parse_args()

    entries = load(a.build)
    if a.match:
        rx = re.compile(a.match, re.I)
        entries = [e for e in entries if rx.search(e["file"])]

    if a.list or not a.match:
        for e in entries:
            print(e["file"])
        print("(%d 个)" % len(entries))
        return

    if not entries:
        sys.exit("没有匹配 %r 的编译单元" % a.match)
    e = entries[0]
    if len(entries) > 1:
        print("[warn] 匹配到 %d 个，用第一个：%s" % (len(entries), e["file"]))

    args = e["command"]
    out_dir = os.path.join(a.build, "intel")
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.splitext(os.path.basename(e["file"]))[0]
    out = a.out or os.path.join(out_dir, "%s.%s" % (base, a.kind))

    cmd = rewrite(args, out, a.kind)
    print("[cc] %s" % e["file"])
    print("[->] %s" % out)
    r = subprocess.run(cmd, cwd=e["directory"], shell=True)
    if r.returncode != 0:
        sys.exit("编译失败 rc=%d" % r.returncode)
    print("[ok] %d 字节" % os.path.getsize(out))


if __name__ == "__main__":
    main()
