#!/bin/bash

# 统一依赖库编译脚本
# 同时编译 Debug 和 Release 版本
# Debug 版本库文件名加 _d 后缀

set -e

CLEAN_BUILD="${1}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 获取 CPU 核心数
if [ -f /proc/cpuinfo ]; then
    NPROC=$(nproc)
else
    NPROC=4
fi

echo "========================================"
echo "编译依赖库 (Debug + Release)"
echo "========================================"
echo "CPU 核心数: $NPROC"
echo ""

DEPS_DIR="$SCRIPT_DIR/deps"
INSTALL_DIR="$DEPS_DIR/install"

# ============================================
# 1. 编译 libsrtp (Debug + Release)
# ============================================
echo "================================================"
echo "1/5: 编译 libsrtp3 (Debug + Release)"
echo "================================================"

cd "$DEPS_DIR/libsrtp"

# Release 版本
echo "编译 libsrtp3 (Release)..."
if [ "$CLEAN_BUILD" = "clean" ]; then
    rm -rf build
fi
mkdir -p build
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-O2 -DNDEBUG" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DENABLE_OPENSSL=OFF \
    -DBUILD_SHARED_LIBS=OFF
make -j$NPROC
make install
cd ..

# Debug 版本
echo "编译 libsrtp3 (Debug)..."
if [ "$CLEAN_BUILD" = "clean" ]; then
    rm -rf build_debug
fi
mkdir -p build_debug
cd build_debug
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-O0 -g3 -ggdb" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DENABLE_OPENSSL=OFF \
    -DBUILD_SHARED_LIBS=OFF
make -j$NPROC
# 安装 Debug 版本，库文件改名加 _d 后缀
cp libsrtp3.a "$INSTALL_DIR/lib/libsrtp3_d.a"

echo "✓ libsrtp3 编译完成"
echo "  Release: $INSTALL_DIR/lib/libsrtp3.a"
echo "  Debug:   $INSTALL_DIR/lib/libsrtp3_d.a"
echo ""

# ============================================
# 2. 编译 usrsctp (Debug + Release)
# ============================================
echo "================================================"
echo "2/5: 编译 usrsctp (Debug + Release)"
echo "================================================"

cd "$DEPS_DIR/usrsctp"

# Release 版本
echo "编译 usrsctp (Release)..."
if [ "$CLEAN_BUILD" = "clean" ]; then
    rm -rf build
fi
mkdir -p build
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-O2 -DNDEBUG" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -Dsctp_build_programs=OFF \
    -Dsctp_build_fuzzer=OFF
make -j$NPROC
make install
cd ..

# Debug 版本
echo "编译 usrsctp (Debug)..."
if [ "$CLEAN_BUILD" = "clean" ]; then
    rm -rf build_debug
fi
mkdir -p build_debug
cd build_debug
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-O0 -g3 -ggdb" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -Dsctp_build_programs=OFF \
    -Dsctp_build_fuzzer=OFF
make -j$NPROC
# 安装 Debug 版本，库文件改名加 _d 后缀
cp usrsctplib/libusrsctp.a "$INSTALL_DIR/lib/libusrsctp_d.a"

echo "✓ usrsctp 编译完成"
echo "  Release: $INSTALL_DIR/lib/libusrsctp.a"
echo "  Debug:   $INSTALL_DIR/lib/libusrsctp_d.a"
echo ""

# ============================================
# 3. 编译 libmetartccore7 (Debug + Release)
# ============================================
echo "================================================"
echo "3/5: 编译 libmetartccore7 (Debug + Release)"
echo "================================================"

cd "$DEPS_DIR/metaRTC/libmetartccore7"

if [ "$CLEAN_BUILD" = "clean" ]; then
    rm -rf build
fi
mkdir -p build
cd build

echo "编译 libmetartccore7 (同时生成 Debug + Release)..."
cmake ..
make -j$NPROC

# 复制到安装目录
cp libmetartccore7.a "$INSTALL_DIR/lib/"
cp libmetartccore7_d.a "$INSTALL_DIR/lib/"

echo "✓ libmetartccore7 编译完成"
echo "  Release: $INSTALL_DIR/lib/libmetartccore7.a"
echo "  Debug:   $INSTALL_DIR/lib/libmetartccore7_d.a"
echo ""

# ============================================
# 4. 编译 libmetartc7 (Debug + Release)
# ============================================
echo "================================================"
echo "4/5: 编译 libmetartc7 (Debug + Release)"
echo "================================================"

cd "$DEPS_DIR/metaRTC/libmetartc7"

if [ "$CLEAN_BUILD" = "clean" ]; then
    rm -rf build
fi
mkdir -p build
cd build

echo "编译 libmetartc7 (同时生成 Debug + Release)..."
cmake .. -DNoCapture=1 -DNoPlayer=1
make -j$NPROC

# 复制到安装目录
cp libmetartc7.a "$INSTALL_DIR/lib/"
cp libmetartc7_d.a "$INSTALL_DIR/lib/"

echo "✓ libmetartc7 编译完成"
echo "  Release: $INSTALL_DIR/lib/libmetartc7.a"
echo "  Debug:   $INSTALL_DIR/lib/libmetartc7_d.a"
echo ""

# ============================================
# 5. 编译 libyangwhip7 (Debug + Release)
# ============================================
echo "================================================"
echo "5/5: 编译 libyangwhip7 (Debug + Release)"
echo "================================================"

cd "$DEPS_DIR/metaRTC/libyangwhip7"

if [ "$CLEAN_BUILD" = "clean" ]; then
    rm -rf build
fi
mkdir -p build
cd build

echo "编译 libyangwhip7 (同时生成 Debug + Release)..."
cmake ..
make -j$NPROC

# 复制到安装目录
cp libyangwhip7.a "$INSTALL_DIR/lib/"
cp libyangwhip7_d.a "$INSTALL_DIR/lib/"

echo "✓ libyangwhip7 编译完成"
echo "  Release: $INSTALL_DIR/lib/libyangwhip7.a"
echo "  Debug:   $INSTALL_DIR/lib/libyangwhip7_d.a"
echo ""

# ============================================
# 完成
# ============================================
echo "================================================"
echo "✓ 所有依赖库编译完成!"
echo "================================================"
echo ""
echo "已安装的库文件:"
ls -lh "$INSTALL_DIR/lib/"*.a | grep -E "(libsrtp3|libusrsctp|libmetartc|libyangwhip)" || true
echo ""
echo "编译完成时间: $(date)"

