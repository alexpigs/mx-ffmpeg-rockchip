#!/bin/bash
# FFmpeg with metaRTC 一键编译脚本

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

echo "========================================"
echo "  FFmpeg with metaRTC WHEP 一键编译"
echo "========================================"
echo ""

# 检查是否在正确的目录
if [ ! -f "configure" ]; then
    print_error "请在 FFmpeg 源码根目录运行此脚本"
    exit 1
fi

# 步骤 1: 编译 metaRTC
print_info "[1/3] 编译 metaRTC..."
echo ""

METARTC_DIR="deps/metaRTC"

if [ ! -d "$METARTC_DIR" ]; then
    print_error "找不到 metaRTC 目录: $METARTC_DIR"
    exit 1
fi

# 库文件路径
METARTCCORE7_LIB="${METARTC_DIR}/libmetartccore7/build"
YANGWHIP7_LIB="${METARTC_DIR}/libyangwhip7/build"
METARTC7_LIB="${METARTC_DIR}/libmetartc7/build"

# 检查 libmetartccore7 是否已编译
if [ ! -f "${METARTCCORE7_LIB}/libmetartccore7.a" ]; then
    print_info "编译 libmetartccore7..."
    cd "${METARTC_DIR}/libmetartccore7"
    chmod +x cmake_x64.sh
    ./cmake_x64.sh
    cd build
    make -j$(nproc 2>/dev/null || echo 4)
    cd - > /dev/null
    cd - > /dev/null
    
    if [ ! -f "${METARTCCORE7_LIB}/libmetartccore7.a" ]; then
        print_error "libmetartccore7 编译失败"
        exit 1
    fi
else
    print_info "✅ libmetartccore7 已存在"
fi

# 检查 libyangwhip7 是否已编译
if [ ! -f "${YANGWHIP7_LIB}/libyangwhip7.a" ]; then
    print_info "编译 libyangwhip7..."
    cd "${METARTC_DIR}/libyangwhip7"
    chmod +x cmake_x64.sh
    ./cmake_x64.sh
    cd build
    make -j$(nproc 2>/dev/null || echo 4)
    cd - > /dev/null
    cd - > /dev/null
    
    if [ ! -f "${YANGWHIP7_LIB}/libyangwhip7.a" ]; then
        print_warn "libyangwhip7 编译失败（可选）"
    fi
else
    print_info "✅ libyangwhip7 已存在"
fi

# 检查 libmetartc7 是否已编译
if [ ! -f "${METARTC7_LIB}/libmetartc7.a" ]; then
    print_info "编译 libmetartc7..."
    cd "${METARTC_DIR}/libmetartc7"
    chmod +x cmake_x64.sh
    ./cmake_x64.sh
    cd build
    make -j$(nproc 2>/dev/null || echo 4)
    cd - > /dev/null
    cd - > /dev/null
    
    if [ ! -f "${METARTC7_LIB}/libmetartc7.a" ]; then
        print_warn "libmetartc7 编译失败（可选）"
    fi
else
    print_info "✅ libmetartc7 已存在"
fi

print_info "✅ metaRTC 编译完成"
echo ""

# 步骤 2: 配置 FFmpeg
print_info "[2/3] 配置 FFmpeg..."
echo ""

if [ -f "configure_with_metartc.sh" ]; then
    chmod +x configure_with_metartc.sh
    ./configure_with_metartc.sh "$@"
else
    print_warn "找不到 configure_with_metartc.sh，使用默认配置"
    
    METARTC_INCLUDE="${METARTC_DIR}/include"
    METARTC_INCLUDE2="${METARTC_DIR}/thirdparty/user_include"
    EXTRA_CFLAGS="-I${METARTC_INCLUDE} -I${METARTC_INCLUDE2}"
    
    # 构建 LDFLAGS - 只包含已存在的库
    EXTRA_LDFLAGS="-L${METARTCCORE7_LIB}"
    EXTRA_LIBS="-lmetartccore7"
    
    # 添加可选库（如果存在）
    if [ -f "${METARTC7_LIB}/libmetartc7.a" ]; then
        EXTRA_LDFLAGS="${EXTRA_LDFLAGS} -L${METARTC7_LIB}"
        EXTRA_LIBS="${EXTRA_LIBS} -lmetartc7"
    fi
    
    if [ -f "${YANGWHIP7_LIB}/libyangwhip7.a" ]; then
        EXTRA_LDFLAGS="${EXTRA_LDFLAGS} -L${YANGWHIP7_LIB}"
        EXTRA_LIBS="${EXTRA_LIBS} -lyangwhip7"
    fi
    
    # 添加系统依赖库
    EXTRA_LIBS="${EXTRA_LIBS} -lssl -lcrypto -lpthread -ldl -lm"
    
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
        --enable-debug=3 \
        --disable-optimizations \
        --disable-stripping \
        --enable-muxer=fifo \
        --enable-openssl \
        --enable-demuxer=whep \
        --extra-cflags="${EXTRA_CFLAGS} -g -O0" \
        --extra-ldflags="${EXTRA_LDFLAGS} -g" \
        --extra-libs="${EXTRA_LIBS}" \
        "$@"
fi

print_info "✅ FFmpeg 配置成功"
echo ""

# 步骤 3: 编译 FFmpeg
print_info "[3/3] 编译 FFmpeg..."
echo ""

JOBS=$(nproc 2>/dev/null || echo 4)
print_info "使用 $JOBS 个并行任务"

make -j$JOBS

echo ""
echo "========================================"
print_info "✅ 编译完成！"
echo "========================================"
echo ""

# 验证 ffmpeg 可执行文件
if [ -f "ffmpeg" ]; then
    print_info "FFmpeg 可执行文件: $(pwd)/ffmpeg"
    ./ffmpeg -version | head -n 1
    echo ""
    
    # 检查 WHEP 支持
    if ./ffmpeg -formats 2>&1 | grep -q "whep"; then
        print_info "✅ WHEP 解复用器已启用"
    else
        print_warn "⚠️  WHEP 解复用器可能未正确编译"
    fi
else
    print_error "编译失败，找不到 ffmpeg 可执行文件"
    exit 1
fi

echo ""
echo "========================================"
echo "使用示例"
echo "========================================"
echo ""
echo "1. 从 SRS 拉取 WHEP 流并播放:"
echo "   ./ffplay -i whep://192.168.1.100:1985/rtc/livestream"
echo ""
echo "2. 从 WHEP 转推到 RTMP:"
echo "   ./ffmpeg -re -i whep://192.168.1.100:1985/rtc/livestream \\"
echo "            -c copy -f flv rtmp://server/live/stream"
echo ""
echo "3. WHEP 转文件:"
echo "   ./ffmpeg -i whep://192.168.1.100:1985/rtc/livestream \\"
echo "            -c copy output.mp4"
echo ""

