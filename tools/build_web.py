#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
构建前钩子：把 web/ 下的页面 gzip 压缩后生成 include/web_assets.h

这样做的好处：
  1. 网页源码仍是可正常编辑/预览的独立文件，不必手工转成 C 字符串；
  2. 资源随固件一起烧录，不依赖 SPIFFS/LittleFS，OTA 升级不会丢页面；
  3. gzip 后体积通常只剩 1/4，浏览器原生解压，速度更快。

由 platformio.ini 中的 extra_scripts = pre:tools/build_web.py 自动调用；
也可以单独运行：python tools/build_web.py
"""

import os
import io
import gzip
import sys

# 需要打包的文件： (源文件名, 生成的 C 符号前缀)
FILES = [
    ("index.html", "INDEX_HTML"),
]


def to_c_array(data: bytes) -> str:
    """把二进制转成带缩进换行的 C 数组初始化串"""
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]))
    return ",\n".join(lines)


def gzip_deterministic(raw: bytes) -> bytes:
    """固定 mtime 压缩，保证内容不变时输出字节一致，避免触发无谓的重新编译"""
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(raw)
    return buf.getvalue()


def build(project_dir: str) -> bool:
    web_dir = os.path.join(project_dir, "web")
    out_path = os.path.join(project_dir, "include", "web_assets.h")

    parts = [
        "// ===========================================================",
        "//  本文件由 tools/build_web.py 自动生成，请勿手工修改！",
        "//  修改网页请编辑 web/ 目录下的源文件，重新编译即可生效。",
        "// ===========================================================",
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "#include <pgmspace.h>",
        "",
    ]

    total_raw = total_gz = 0
    for fname, symbol in FILES:
        src = os.path.join(web_dir, fname)
        if not os.path.exists(src):
            print("[web] 错误：找不到 %s" % src)
            return False
        with open(src, "rb") as f:
            raw = f.read()
        gz = gzip_deterministic(raw)
        total_raw += len(raw)
        total_gz += len(gz)

        parts.append("// %s : 原始 %d 字节 -> gzip %d 字节 (压缩率 %.1f%%)"
                     % (fname, len(raw), len(gz), 100.0 * len(gz) / max(1, len(raw))))
        parts.append("const uint8_t %s_GZ[] PROGMEM = {" % symbol)
        parts.append(to_c_array(gz))
        parts.append("};")
        parts.append("const size_t %s_GZ_LEN = %d;" % (symbol, len(gz)))
        parts.append("")

    content = "\n".join(parts)

    old = ""
    if os.path.exists(out_path):
        with open(out_path, "r", encoding="utf-8") as f:
            old = f.read()

    if old != content:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(content)
        print("[web] 已生成 include/web_assets.h  （原始 %d B -> gzip %d B）"
              % (total_raw, total_gz))
    else:
        print("[web] 网页资源无变化，跳过生成  （gzip %d B）" % total_gz)
    return True


# ---- PlatformIO 构建环境入口 ----
try:
    Import("env")            # noqa: F821  (PlatformIO 注入)
    _pio = True
except Exception:
    _pio = False

if _pio:
    if not build(env["PROJECT_DIR"]):     # noqa: F821
        raise RuntimeError("网页资源打包失败")
elif __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sys.exit(0 if build(root) else 1)
