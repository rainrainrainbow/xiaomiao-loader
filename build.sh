#!/bin/sh
# 一键构建：编译固件 + 生成 merged.bin
set -e

. ~/esp/esp-idf/export.sh

idf.py build
idf.py merge-bin -o xiaomiao-loader-merged.bin

echo ""
echo "Done. Output:"
echo "  build/xiaomiao-loader.bin              (app only)"
echo "  build/xiaomiao-loader-merged.bin       (full flash image)"
