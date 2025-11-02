# 更新日志

## 2025-11-01 - 修正 metaRTC 库路径

### 🔧 修改内容

根据实际的 metaRTC 库结构，更新了编译配置脚本，正确指向库文件的实际位置。

### 📝 主要变更

#### 1. **configure_with_metartc.sh** - FFmpeg 配置脚本

**修改前**:
```bash
METARTC_LIB="${METARTC_DIR}/bin/lib_debug"
```

**修改后**:
```bash
METARTCCORE7_LIB="${METARTC_DIR}/libmetartccore7/build"
YANGWHIP7_LIB="${METARTC_DIR}/libyangwhip7/build"
METARTC7_LIB="${METARTC_DIR}/libmetartc7/build"
```

**变更说明**:
- ✅ 使用实际的库文件路径（各自的 `build/` 目录）
- ✅ 分别检查每个库文件的存在
- ✅ 提供更清晰的错误提示

#### 2. **build_with_metartc.sh** - 一键编译脚本

**修改前**:
- 假设存在 `quick_build.sh` 统一编译脚本
- 库文件在 `bin/lib_debug/` 目录

**修改后**:
- ✅ 分别编译三个库: `libmetartccore7`, `libyangwhip7`, `libmetartc7`
- ✅ 每个库使用各自的 `cmake_x64.sh` 编译脚本
- ✅ 检查已存在的库，避免重复编译
- ✅ 使用正确的库文件路径

**新增逻辑**:
```bash
# 编译 libmetartccore7
cd "${METARTC_DIR}/libmetartccore7"
./cmake_x64.sh
cd build && make -j$(nproc)

# 编译 libyangwhip7
cd "${METARTC_DIR}/libyangwhip7"
./cmake_x64.sh
cd build && make -j$(nproc)

# 编译 libmetartc7
cd "${METARTC_DIR}/libmetartc7"
./cmake_x64.sh
cd build && make -j$(nproc)
```

#### 3. **新增文档** - deps/metaRTC/LIBRARY_STRUCTURE.md

**内容**:
- 📖 详细说明 metaRTC 库的实际结构
- 📖 每个库的编译方法
- 📖 头文件和库文件的位置
- 📖 FFmpeg 集成配置说明
- 📖 完整的编译流程
- 📖 故障排查指南

---

### 📂 实际的库文件位置

```
deps/metaRTC/
├── libmetartccore7/
│   └── build/
│       └── libmetartccore7.a          ⭐ (核心库)
│
├── libyangwhip7/
│   └── build/
│       └── libyangwhip7.a             ⭐ (WHIP/WHEP)
│
└── libmetartc7/
    └── build/
        └── libmetartc7.a              ⭐ (高级 API)
```

**注意**: 
- ❌ **不是** `deps/metaRTC/bin/lib_debug/*.a`
- ✅ **而是** `deps/metaRTC/libXXX/build/*.a`

---

### 🔍 为什么要修改

#### 原因
1. **错误假设**: 之前假设 metaRTC 有统一的编译脚本和输出目录
2. **实际情况**: metaRTC 由三个独立的库组成，各自编译到自己的 `build/` 目录
3. **用户反馈**: 用户指出 `libmetartccore7.a` 在 `deps/metaRTC/libmetartccore7`

#### 影响
- **修改前**: 编译脚本会找不到库文件，导致配置失败
- **修改后**: 脚本能正确找到并链接库文件

---

### ✅ 修改验证

#### 1. 检查库文件

```bash
# 检查 libmetartccore7
ls -lh deps/metaRTC/libmetartccore7/build/libmetartccore7.a

# 检查 libyangwhip7
ls -lh deps/metaRTC/libyangwhip7/build/libyangwhip7.a

# 检查 libmetartc7
ls -lh deps/metaRTC/libmetartc7/build/libmetartc7.a
```

#### 2. 测试配置脚本

```bash
./configure_with_metartc.sh
```

**预期输出**:
```
找到 metaRTC 核心库:
-rw-r--r-- 1 user user 2.5M Nov  1 12:00 deps/metaRTC/libmetartccore7/build/libmetartccore7.a

找到 yangwhip7 库:
-rw-r--r-- 1 user user 150K Nov  1 12:00 deps/metaRTC/libyangwhip7/build/libyangwhip7.a

找到 metartc7 库:
-rw-r--r-- 1 user user 3.2M Nov  1 12:00 deps/metaRTC/libmetartc7/build/libmetartc7.a
```

#### 3. 测试一键编译

```bash
./build_with_metartc.sh
```

**预期行为**:
- ✅ 自动编译缺失的 metaRTC 库
- ✅ 跳过已存在的库（加快编译）
- ✅ 正确配置 FFmpeg
- ✅ 成功编译 FFmpeg

---

### 📚 相关文档更新

以下文档已更新以反映正确的库路径：

1. ✅ `configure_with_metartc.sh` - 配置脚本
2. ✅ `build_with_metartc.sh` - 编译脚本
3. ✅ `deps/metaRTC/LIBRARY_STRUCTURE.md` - 新增库结构说明
4. ✅ `CHANGELOG.md` - 本文档

**原有文档仍然有效**:
- ✅ `README_WHEP.md` - 项目主页
- ✅ `QUICK_START.md` - 快速开始
- ✅ `WHEP_USAGE.md` - 使用指南
- ✅ `WHEP_IMPLEMENTATION.md` - 技术实现
- ✅ `PROJECT_SUMMARY.md` - 项目总结
- ✅ `WHEP_INDEX.md` - 完整索引

---

### 🚀 使用建议

#### 新用户（从零开始）

推荐使用一键编译脚本：

```bash
# 一步到位
./build_with_metartc.sh
```

脚本会自动：
1. 检查并编译 metaRTC 库
2. 配置 FFmpeg
3. 编译 FFmpeg
4. 验证 WHEP 支持

#### 高级用户（手动控制）

可以分步执行：

```bash
# 步骤 1: 手动编译 metaRTC
cd deps/metaRTC/libmetartccore7
./cmake_x64.sh && cd build && make -j$(nproc)
cd ../../..

cd deps/metaRTC/libyangwhip7
./cmake_x64.sh && cd build && make -j$(nproc)
cd ../../..

cd deps/metaRTC/libmetartc7
./cmake_x64.sh && cd build && make -j$(nproc)
cd ../../..

# 步骤 2: 配置 FFmpeg
./configure_with_metartc.sh

# 步骤 3: 编译 FFmpeg
make -j$(nproc)
```

---

### 🐛 已知问题

#### Windows 平台

**问题**: 脚本使用 Bash 语法，在 Windows CMD 中无法直接运行

**解决方案**:
1. 使用 Git Bash 或 WSL
2. 或手动执行编译命令（参考 `LIBRARY_STRUCTURE.md`）

#### ARM 平台

**问题**: 编译脚本默认使用 `cmake_x64.sh`

**解决方案**:
- ARM32: 使用 `cmake_arm.sh`
- ARM64/aarch64: 使用 `cmake_aarch64.sh` 或修改脚本

---

### 📞 技术支持

如果遇到问题，请参考：

1. **库结构说明**: `deps/metaRTC/LIBRARY_STRUCTURE.md`
2. **快速开始**: `QUICK_START.md` 的"故障排查"章节
3. **详细用法**: `WHEP_USAGE.md` 的"常见问题"章节

---

### 📝 备注

**重要提醒**:

1. ⚠️ **必需库**: `libmetartccore7.a` 是必需的（核心 WebRTC 功能）
2. ⚠️ **推荐库**: `libyangwhip7.a` 推荐编译（WHIP/WHEP 协议支持）
3. ℹ️ **可选库**: `libmetartc7.a` 可选（高级 API，WHEP 解复用器可能不需要）

**编译顺序**:

1. 先编译 `libmetartccore7`（基础）
2. 再编译 `libyangwhip7`（依赖 core7）
3. 最后编译 `libmetartc7`（依赖前两者）

---

## 历史版本

### v1.0 (2025-11-01)

- ✅ 初始版本发布
- ✅ 完整的 WHEP 解复用器实现
- ✅ 完整的文档体系
- ✅ 编译脚本和示例

### v1.0.1 (2025-11-01) - 本次更新

- 🔧 修正 metaRTC 库路径配置
- 📝 新增 `LIBRARY_STRUCTURE.md`
- 📝 更新编译脚本
- 📝 新增 `CHANGELOG.md`

---

**最后更新**: 2025-11-01

