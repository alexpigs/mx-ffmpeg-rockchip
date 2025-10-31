#!/bin/bash
#
# 编译 libdatachannel 动态库
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LDC_DIR="$SCRIPT_DIR/libdatachannel"
BUILD_DIR="$LDC_DIR/build"
INSTALL_DIR="$SCRIPT_DIR/install"

echo "=== 编译 libdatachannel ==="
echo "源码目录: $LDC_DIR"
echo "构建目录: $BUILD_DIR"
echo "安装目录: $INSTALL_DIR"

# 清理旧的构建
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_DIR"

cd "$BUILD_DIR"

# 配置 CMake - 编译动态库
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DNO_WEBSOCKET=ON \
    -DNO_EXAMPLES=ON \
    -DNO_TESTS=ON \
    -DUSE_GNUTLS=OFF \
    -DUSE_NICE=OFF

# 编译
make -j$(nproc)

# 安装
make install

echo ""
echo "=== 编译完成 ==="
echo "头文件位置: $INSTALL_DIR/include"
echo "动态库位置: $INSTALL_DIR/lib/libdatachannel.so"
echo ""
echo "现在可以运行 configure 配置 FFmpeg:"
echo "./configure --enable-libdatachannel ..."

