# ELF2 双 Profile 交叉编译环境设计

## 1. 背景

`rk3588` 当前能够使用 ELF2 官方 Buildroot SDK 编译出 AArch64 程序，但该产物属于降级编译版本：Paho MQTT C 与 libmodbus 未启用，FFmpeg 为 4.x 且没有 `h264_rkmpp`。从 `ubuntu1` 提取出的材料包含 GCC 10.3、Qt SDK、FFmpeg 6 运行库、MPP 头文件/运行库，以及 Paho/libmodbus 的 arm64 开发包，但这些材料分散在不同目录，ABI 基线也不一致，不能直接拼成可靠 sysroot。

F 盘资料中存在 ELF2 Desktop `update.img`，对应厂商 Jammy rootfs。目标板业务实际运行在 Desktop/Jammy 环境，因此真实部署产物必须以该 rootfs 为 ABI 基线；Buildroot SDK 继续保留，仅用于快速编译检查。

## 2. 目标与非目标

### 2.1 目标

1. 建立两个含义明确、互不污染的构建 Profile：
   - `buildroot-check`：快速编译检查。
   - `jammy-deploy`：面向 ELF2 Desktop/Jammy 的完整部署构建。
2. `jammy-deploy` 必须启用 Paho MQTT C、libmodbus、RKNN、RGA、MPP，以及带 `h264_rkmpp` 和 AAC 编码能力的 FFmpeg 6。
3. 环境可以从 F 盘镜像和已校验迁移归档重新生成，不依赖 `/home/c`、`/home/why` 等历史绝对路径。
4. 构建过程默认净化 Windows PATH，避免 Anaconda Qt 等 Windows 工具进入 WSL 构建链。
5. 为环境组装、依赖闭包和最终产物提供可重复执行的验收命令。

### 2.2 非目标

1. 本阶段不修改摄像头、推流、AI 或音频业务逻辑。
2. 本阶段不重编完整内核、U-Boot 或整套 Rockchip 固件。
3. 不把数 GB 的 sysroot、工具链或二进制库提交到 Git。
4. 未完成板端回归前不删除 `ubuntu1`。

## 3. 总体架构

环境安装在用户目录，项目仓库只保存脚本、Profile 配置、测试和 manifest。

```text
~/.local/opt/elf2-sdk/
├── toolchains/
│   └── gcc-arm-10.3-aarch64/
├── profiles/
│   ├── buildroot-check/
│   │   └── sdk -> 现有 Buildroot SDK
│   └── jammy-deploy/
│       ├── sysroot/          # 从 Desktop 镜像生成的统一 sysroot
│       ├── sources/          # 已校验源码包或其只读引用
│       ├── build/            # FFmpeg 等可删除的中间构建目录
│       └── manifests/        # 来源、哈希、版本与文件清单
└── current -> profiles/buildroot-check
```

项目侧提供统一入口：

```bash
./scripts/build_elf2.sh --profile buildroot-check
./scripts/build_elf2.sh --profile jammy-deploy
./scripts/verify_elf2_sdk.sh --profile jammy-deploy
```

脚本只接受已知 Profile，不使用“发现什么就链接什么”的隐式行为。

## 4. Profile 设计

### 4.1 `buildroot-check`

该 Profile 复用现有 Buildroot SDK，目的仅为快速验证源码可编译、RKNN/RGA API 可见和基本链接正确。

- 允许 Paho、libmodbus、`h264_rkmpp` 缺失。
- 缺失项必须输出明确 warning。
- 产物不得标记为部署版本。
- 继续作为日常快速回归环境，避免每次都处理较大的 Jammy sysroot。

### 4.2 `jammy-deploy`

该 Profile 以 F 盘 Desktop `update.img` 内的 Jammy rootfs 为唯一 ABI 基线。

- 使用迁移出的 GCC 10.3 AArch64 工具链。
- 从 rootfs 保留动态加载器、glibc、系统运行库和目标板厂商运行库。
- 缺少的开发头文件通过版本匹配的 arm64 `-dev` 包、厂商 SDK 头文件或项目已归档开发包补齐。
- Paho/libmodbus 使用现有 arm64 `.deb` 解包到 sysroot，并记录包版本及 SHA-256。
- RKNN、RGA、MPP 的头文件和库必须与 Desktop 镜像中的运行版本核对；不能无条件从 Buildroot Profile 拷贝。
- 任一必需依赖缺失时立即失败，不允许编译 ENOSYS 降级实现。

## 5. Jammy sysroot 生成流程

1. 校验 Desktop `update.img` 的厂商 MD5。
2. 使用 Rockchip 固件解包工具提取 `jammy-rootfs.img`。如果当前资料包不含解包工具，则在删除 VM 前，从完整 ELF2 SDK 中仅提取 Rockchip `Linux_Pack_Firmware` 工具目录。
3. 只读挂载 rootfs 镜像，读取 `/etc/os-release`、架构、glibc 版本及厂商库版本。
4. 将构建所需目录复制到临时 sysroot，保留符号链接、权限和 multiarch 目录结构。
5. 解包匹配版本的开发包和厂商头文件。
6. 生成 manifest，记录所有输入文件来源、哈希、版本和最终 sysroot 大小。
7. 原子替换正式 sysroot；中途失败不得留下“半可用”安装目录。

sysroot 组装脚本必须可重复执行。重复运行相同输入应生成相同的版本清单，不修改 F 盘原始镜像。

## 6. 最小 FFmpeg 6 + MPP

现有 `libavcodec.so.60` 虽包含 `h264_rkmpp`，但没有开发头文件，并依赖大量无关第三方库，不能直接作为开发环境。`jammy-deploy` 将从已校验的 FFmpeg 6.0 源码重新构建最小共享库集合。

保留能力：

- `libavcodec`、`libavformat`、`libavutil`、`libswresample`；
- Rockchip MPP H.264 编码器 `h264_rkmpp`；
- FFmpeg 原生 AAC 编码器；
- RTMP/TCP/file 等项目实际使用的协议；
- FLV、MPEG-TS 等项目实际使用的封装；
- 项目当前编码和推流链路需要的 parser、bitstream filter 与基础 filter。

关闭命令行程序、文档和无关 codec/外部库，降低运行时依赖闭包。FFmpeg 安装到 `jammy-deploy/sysroot/usr/local`，项目链接和板端部署均使用同一套产物。构建配置、源码哈希和 `ffmpeg -buildconf` 等价信息写入 manifest。

## 7. 项目构建接口

Makefile 保留现有默认行为，但新增显式 Profile 层：

```bash
make PROFILE=buildroot-check
make PROFILE=jammy-deploy
make PROFILE=jammy-deploy print-config
make PROFILE=jammy-deploy check-env
```

Profile 配置负责提供：

- `CROSS_COMPILE`
- `SYSROOT`
- 额外 include/lib 路径
- 必需功能开关
- 运行时 `rpath-link`

`jammy-deploy check-env` 必须验证：

```text
PAHO_MQTT=enabled
LIBMODBUS=enabled
RKMPP_ENCODER=available
RKNN=available
RGA=available
MPP=available
FFMPEG_MAJOR=6
```

任何一项不满足即返回非零状态。源码中的降级实现只服务 `buildroot-check`，不能进入部署产物。

## 8. PATH、错误处理和可恢复性

- 所有构建入口使用固定 Linux PATH，不修改全局 `/etc/wsl.conf`。
- 脚本启动时打印实际使用的编译器、sysroot 和 Profile。
- 检测到 Windows `qmake.exe`、Windows DLL 或历史 `/home/c`、`/home/why` 路径时立即失败。
- 下载或解包失败时保留日志，但删除不完整的临时安装目录。
- 所有删除动作仅允许发生在脚本创建且路径前缀已校验的临时目录。
- 不覆盖用户现有 SDK；正式切换通过符号链接或原子重命名完成。

## 9. 测试与验收

### 9.1 自动化测试

1. Profile 解析测试：未知 Profile 必须失败。
2. 环境检查测试：分别验证缺少 Paho、libmodbus、FFmpeg 6/MPP 时会正确失败。
3. 头文件/链接冒烟测试：分别编译调用 MQTTClient、libmodbus、FFmpeg、RKNN、RGA、MPP 的最小程序。
4. 完整工程干净构建：在临时副本中构建，避免依赖旧对象文件。
5. ELF 检查：产物必须为 AArch64，解释器和 SONAME 必须来自 Jammy Profile。
6. 依赖闭包检查：所有 `DT_NEEDED` 均能在 Jammy sysroot 中解析。
7. glibc 版本检查：产物要求的最高 `GLIBC_*` 版本不得高于 Desktop rootfs 提供版本。

部署产物至少应包含以下依赖：

```text
libavformat.so.60
libavcodec.so.60
libavutil.so.58
libswresample.so.4
librockchip_mpp.so.1
libpaho-mqtt3c.so.1
libmodbus.so.5
librknnrt.so
librga.so.2
```

### 9.2 板端回归

板端至少验证：

1. 摄像头 V4L2/DMA-BUF 采集；
2. RKNN 推理和 RGA 图像处理；
3. `h264_rkmpp` 编码与 RTMP 推流；
4. AAC 编码能力的独立冒烟测试，为后续 ALSA 音频链路做准备；
5. Paho MQTT 连接、发布和离线恢复；
6. libmodbus 串口采集；
7. 断网落盘和恢复推流。

没有开发板时，自动化检查只能证明可构建和 ABI 闭包完整，不能替代板端驱动、设备节点和实时性能验证。

## 10. `ubuntu1` 删除门槛

只有同时满足以下条件才允许删除：

1. 迁移归档 SHA-256 全部通过；
2. Rockchip 镜像解包工具及必要 SDK 头文件已独立保存；
3. 从归档和 F 盘镜像可以重新生成两个 Profile；
4. `jammy-deploy` 完整工程干净构建通过；
5. 依赖闭包和 glibc 版本检查通过；
6. 板端核心链路回归通过；
7. 生成最终环境 manifest 和恢复说明。

删除虚拟机属于独立操作，不由 SDK 安装脚本自动触发。

## 11. 实施边界

本设计拆为四个顺序阶段：

1. Profile 与失败优先测试；
2. Desktop/Jammy sysroot 提取和 manifest；
3. Paho、libmodbus、MPP/RKNN/RGA 与最小 FFmpeg 6 组装；
4. 完整工程、依赖闭包和板端验收。

前三阶段可以在无开发板条件下完成。第四阶段中的板端回归等待硬件可用后执行，在此之前虚拟机保持关机但不删除。
