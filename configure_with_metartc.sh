#!/bin/bash
# FFmpeg with metaRTC WHEP 支持编译配置脚本

set -e

# 颜色输出
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}配置 FFmpeg 编译（集成 metaRTC WHEP 支持）${NC}"
echo ""

# metaRTC 路径
METARTC_DIR="$(pwd)/deps/metaRTC"
METARTC_INCLUDE="${METARTC_DIR}/include"
METARTC_INCLUDE2="${METARTC_DIR}/thirdparty/user_include"

# 库文件路径（从各自的 build 目录）
METARTCCORE7_LIB="${METARTC_DIR}/libmetartccore7/build"
YANGWHIP7_LIB="${METARTC_DIR}/libyangwhip7/build"
METARTC7_LIB="${METARTC_DIR}/libmetartc7/build"

# 依赖库路径（libsrtp 和 usrsctp）
DEPS_LIB="$(pwd)/deps/install/lib"
DEPS_INCLUDE="$(pwd)/deps/install/include"

# 检查 metaRTC 是否已编译
if [ ! -f "${METARTCCORE7_LIB}/libmetartccore7.a" ]; then
    echo -e "${YELLOW}警告: libmetartccore7.a 未找到${NC}"
    echo "请先编译 metaRTC 库："
    echo "  cd deps/metaRTC/libmetartccore7"
    echo "  chmod +x cmake_x64.sh && ./cmake_x64.sh"
    echo "  cd build && make -j\$(nproc)"
    exit 1
fi

echo -e "${GREEN}找到 metaRTC 核心库:${NC}"
ls -lh "${METARTCCORE7_LIB}/libmetartccore7.a"

# 检查可选库（不强制要求）
if [ -f "${YANGWHIP7_LIB}/libyangwhip7.a" ]; then
    echo -e "${GREEN}找到 yangwhip7 库:${NC}"
    ls -lh "${YANGWHIP7_LIB}/libyangwhip7.a"
fi

if [ -f "${METARTC7_LIB}/libmetartc7.a" ]; then
    echo -e "${GREEN}找到 metartc7 库:${NC}"
    ls -lh "${METARTC7_LIB}/libmetartc7.a"
fi
echo ""

# 构建额外的 CFLAGS 和 LDFLAGS
EXTRA_CFLAGS="-I${METARTC_INCLUDE} -I${METARTC_INCLUDE2} -I${DEPS_INCLUDE}"

# 构建 LDFLAGS - 添加所有库路径
EXTRA_LDFLAGS="-L${YANGWHIP7_LIB} -L${METARTC7_LIB} -L${METARTCCORE7_LIB} -L${DEPS_LIB}"

# 静态库链接顺序：依赖者在前，被依赖者在后
EXTRA_LIBS=""

# 1. 最高层：libyangwhip7（依赖 libmetartc7 和 libmetartccore7）
if [ -f "${YANGWHIP7_LIB}/libyangwhip7.a" ]; then
    EXTRA_LIBS="${EXTRA_LIBS} -lyangwhip7"
fi

# 2. 中间层：libmetartc7（依赖 libmetartccore7）
if [ -f "${METARTC7_LIB}/libmetartc7.a" ]; then
    EXTRA_LIBS="${EXTRA_LIBS} -lmetartc7"
fi

# 3. 基础层：libmetartccore7（被所有库依赖）
EXTRA_LIBS="${EXTRA_LIBS} -lmetartccore7"

# 添加 libsrtp3 和 usrsctp 静态库（在系统库之前）
if [ -f "${DEPS_LIB}/libsrtp3.a" ]; then
    EXTRA_LIBS="${EXTRA_LIBS} -lsrtp3"
    echo -e "${GREEN}找到 libsrtp3 静态库${NC}"
fi

if [ -f "${DEPS_LIB}/libusrsctp.a" ]; then
    EXTRA_LIBS="${EXTRA_LIBS} -lusrsctp"
    echo -e "${GREEN}找到 libusrsctp 静态库${NC}"
fi

# 添加系统依赖库
EXTRA_LIBS="${EXTRA_LIBS} -lssl -lcrypto -lpthread -ldl -lm"

echo -e "${GREEN}配置 FFmpeg...${NC}"
echo ""

# 运行 configure
# 注意: whep-demuxer 会自动从 libavformat/allformats.c 中检测到
# 如果需要显式禁用某个解复用器，使用 --disable-demuxer=whep
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

echo ""
echo -e "${GREEN}配置完成！${NC}"
echo ""
echo "现在可以运行以下命令编译:"
echo "  make -j\$(nproc)"
echo ""
echo "测试 WHEP 拉流:"
echo "  ./ffmpeg -re -i whep://192.168.1.100:1985/rtc/livestream -c copy output.mp4"
echo ""

