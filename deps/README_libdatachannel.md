# libdatachannel 静态编译集成说明

## 1. 编译 libdatachannel 静态库

首先需要编译 libdatachannel 及其依赖为静态库：

```bash
cd deps
chmod +x build_libdatachannel.sh
./build_libdatachannel.sh
```

编译完成后，会在 `deps/install/` 目录下生成：
- `include/` - 头文件
- `lib/` - 静态库文件
  - `libdatachannel.a`
  - `libjuice.a` (ICE 库)
  - `libusrsctp.a` (SCTP 库)

## 2. 配置 FFmpeg

编译 libdatachannel 后，配置 FFmpeg 时添加 `--enable-libdatachannel` 选项：

```bash
./configure \
    --prefix=./out \
    --enable-gpl \
    --enable-version3 \
    --enable-libdrm \
    --enable-rkmpp \
    --enable-rkrga \
    --enable-debug=3 \
    --disable-optimizations \
    --disable-stripping \
    --extra-cflags="-g -O0" \
    --extra-ldflags="-g" \
    --enable-muxer=fifo \
    --enable-openssl \
    --enable-libdatachannel
```

**注意：** `--enable-libdatachannel` 必须在 `--enable-openssl` 之后，因为 libdatachannel 依赖 OpenSSL。

## 3. 编译 FFmpeg

```bash
make clean
make -j$(nproc)
```

## 4. 验证

编译成功后，可以使用 WHEP demuxer：

```bash
# 播放 WHEP 流
./ffplay -f whep http://your-srs-server:1985/rtc/v1/whep/?app=live&stream=livestream

# 或者使用 ffmpeg 拉流
./ffmpeg -f whep -i http://your-srs-server:1985/rtc/v1/whep/?app=live&stream=livestream output.mp4
```

## 依赖关系

libdatachannel 依赖以下库（已包含在静态编译中）：
- **libjuice** - ICE 库
- **libusrsctp** - SCTP 用户态库
- **OpenSSL** - DTLS 加密（系统库）
- **pthread** - 多线程支持（系统库）

## 目录结构

```
deps/
├── libdatachannel/          # 源码
│   ├── include/
│   ├── src/
│   └── deps/                # 子依赖源码
│       ├── libjuice/
│       ├── libsrtp/
│       ├── usrsctp/
│       └── plog/
├── install/                 # 编译安装目录
│   ├── include/
│   │   └── rtc/            # libdatachannel 头文件
│   └── lib/
│       ├── libdatachannel.a
│       ├── libjuice.a
│       └── libusrsctp.a
└── build_libdatachannel.sh  # 编译脚本
```

## 故障排除

### 编译 libdatachannel 失败

确保安装了必要的依赖：

```bash
# Ubuntu/Debian
sudo apt-get install cmake build-essential libssl-dev

# 或者更完整的依赖
sudo apt-get install cmake g++ pkg-config libssl-dev
```

### FFmpeg configure 找不到 libdatachannel

检查静态库是否存在：

```bash
ls -lh deps/install/lib/libdatachannel.a
ls -lh deps/install/include/rtc/rtc.h
```

如果文件不存在，重新运行编译脚本：

```bash
cd deps
./build_libdatachannel.sh
```

### 链接错误

如果出现链接错误，可能是缺少系统库：

```bash
# 安装 OpenSSL 开发库
sudo apt-get install libssl-dev

# 检查是否有 pthread
ldconfig -p | grep pthread
```

