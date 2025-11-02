#!/bin/bash

# FFmpeg 智能编译脚本
# 用法: ./build.sh [release|debug] [clean]
#   release - 编译优化版本 (默认)
#   debug   - 编译调试版本
#   clean   - 清理后重新编译

set -e

# 解析参数
BUILD_MODE="release"
CLEAN_BUILD=""

for arg in "$@"; do
    case "$arg" in
        release|debug)
            BUILD_MODE="$arg"
            ;;
        clean)
            CLEAN_BUILD="clean"
            ;;
        *)
            echo "未知参数: $arg"
            echo "用法: ./build.sh [release|debug] [clean]"
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 获取 CPU 核心数
if [ -f /proc/cpuinfo ]; then
    NPROC=$(nproc)
else
    NPROC=4
fi

# 颜色输出
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "========================================"
echo "FFmpeg 智能编译系统"
echo "========================================"
echo "编译模式: $BUILD_MODE"
if [ "$CLEAN_BUILD" = "clean" ]; then
    echo "清理模式: 是"
fi
echo ""

DEPS_DIR="$SCRIPT_DIR/deps"
INSTALL_DIR="$DEPS_DIR/install"
METARTC_DIR="$DEPS_DIR/metaRTC"

# ============================================
# 步骤 1: 检查并编译依赖库
# ============================================
echo -e "${GREEN}步骤 1/2: 检查依赖库${NC}"
echo "================================================"

# 检查是否需要编译依赖库
NEED_BUILD_DEPS=0

# 检查 Release 版本库
if [ ! -f "$INSTALL_DIR/lib/libsrtp3.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libusrsctp.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libmetartccore7.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libmetartc7.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libyangwhip7.a" ]; then
    echo -e "${YELLOW}缺少 Release 版本依赖库${NC}"
    NEED_BUILD_DEPS=1
fi

# 检查 Debug 版本库
if [ ! -f "$INSTALL_DIR/lib/libsrtp3_d.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libusrsctp_d.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libmetartccore7_d.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libmetartc7_d.a" ] || \
   [ ! -f "$INSTALL_DIR/lib/libyangwhip7_d.a" ]; then
    echo -e "${YELLOW}缺少 Debug 版本依赖库${NC}"
    NEED_BUILD_DEPS=1
fi

if [ "$NEED_BUILD_DEPS" = "1" ] || [ "$CLEAN_BUILD" = "clean" ]; then
    echo "编译依赖库 (Debug + Release)..."
    ./build_deps.sh "$CLEAN_BUILD"
else
    echo -e "${GREEN}✓ 所有依赖库已存在${NC}"
    echo ""
    echo "Release 库:"
    ls -lh "$INSTALL_DIR/lib/"lib*.a | grep -v "_d.a" || true
    echo ""
    echo "Debug 库:"
    ls -lh "$INSTALL_DIR/lib/"*_d.a || true
fi

echo ""

# ============================================
# 步骤 2: 配置并编译 FFmpeg
# ============================================

# 通用路径
METARTC_INCLUDE="${METARTC_DIR}/include"
METARTC_INCLUDE2="${METARTC_DIR}/thirdparty/user_include"
METARTCCORE7_LIB="${METARTC_DIR}/libmetartccore7/build"
YANGWHIP7_LIB="${METARTC_DIR}/libyangwhip7/build"
METARTC7_LIB="${METARTC_DIR}/libmetartc7/build"
DEPS_LIB="$INSTALL_DIR/lib"
DEPS_INCLUDE="$INSTALL_DIR/include"

EXTRA_CFLAGS="-I${METARTC_INCLUDE} -I${METARTC_INCLUDE2} -I${DEPS_INCLUDE}"
EXTRA_LDFLAGS="-L${YANGWHIP7_LIB} -L${METARTC7_LIB} -L${METARTCCORE7_LIB} -L${DEPS_LIB}"

# 根据 BUILD_MODE 选择库和编译选项
if [ "$BUILD_MODE" = "debug" ]; then
    echo -e "${GREEN}步骤 2/2: 配置并编译 FFmpeg (Debug)${NC}"
    echo "================================================"
    
    # Debug 模式：链接 *_d.a 库
    EXTRA_LIBS="-lyangwhip7_d -lmetartc7_d -lmetartccore7_d -lsrtp3_d -lusrsctp_d -lssl -lcrypto -lpthread -ldl -lm"
    CFLAGS_OPT="-O0 -g3 -ggdb"
    OUTPUT_NAME="ffmpeg_g"
else
    echo -e "${GREEN}步骤 2/2: 配置并编译 FFmpeg (Release)${NC}"
    echo "================================================"
    
    # Release 模式：链接普通库
    EXTRA_LIBS="-lyangwhip7 -lmetartc7 -lmetartccore7 -lsrtp3 -lusrsctp -lssl -lcrypto -lpthread -ldl -lm"
    CFLAGS_OPT="-O2 -DNDEBUG"
    OUTPUT_NAME="ffmpeg"
fi

if [ "$CLEAN_BUILD" = "clean" ] || [ ! -f "config.h" ]; then
    echo "配置 FFmpeg ($BUILD_MODE 模式)..."
    
    ./configure \
        --prefix=./out \
        --enable-gpl \
        --enable-version3 \
        --enable-nonfree \
        --enable-static \
        --disable-shared \
        --enable-libdrm \
        --enable-rkmpp \
        --enable-rkrga \
        --enable-muxer=fifo \
        --enable-openssl \
        --enable-demuxer=whep \
        --extra-cflags="${EXTRA_CFLAGS} ${CFLAGS_OPT}" \
        --extra-ldflags="${EXTRA_LDFLAGS}" \
        --extra-libs="${EXTRA_LIBS}"
fi

echo "编译 FFmpeg ($BUILD_MODE 模式)..."
if [ "$CLEAN_BUILD" = "clean" ]; then
    make clean || true
fi
make -j$NPROC

echo ""
echo -e "${GREEN}✓ FFmpeg ($BUILD_MODE) 编译完成${NC}"
if [ -f "$OUTPUT_NAME" ]; then
    ls -lh "$OUTPUT_NAME"
fi
echo ""

# ============================================
# 完成
# ============================================
echo "================================================"
echo -e "${GREEN}✓ 编译完成！${NC}"
echo "================================================"
echo ""
echo "可执行文件: $OUTPUT_NAME ($BUILD_MODE 模式)"
if [ -f "$OUTPUT_NAME" ]; then
    ls -lh "$OUTPUT_NAME"
fi
echo ""
echo "测试命令:"
echo "  ./$OUTPUT_NAME -formats | grep whep"
echo ""
if [ "$BUILD_MODE" = "debug" ]; then
    echo "GDB 调试:"
    echo "  gdb --args ./$OUTPUT_NAME -loglevel debug -f whep -i \"http://...\" ..."
    echo ""
fi
echo "编译完成时间: $(date)"

