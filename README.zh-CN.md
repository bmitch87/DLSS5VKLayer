# DLSS5VKLayer

[English](README.md) · **简体中文**

DLSS5VKLayer 是一个 Linux Vulkan 层加助手服务，它把呈现的帧转发给运行在 Wine 或自定义 Proton 兼容性工具下的 Windows NGX 神经渲染助手。

本项目处于实验阶段，目标是本地测试与研究。

## 功能特性

- 面向原生 Linux 游戏与 Proton 游戏的 Vulkan 隐式层。
- 失败即放行（fail-open）设计：如果助手缺失或神经初始化失败，游戏会继续呈现原始帧。
- Windows NGX 助手可运行于：
  - 自定义 Steam 兼容性工具，例如 Proton-CachyOS、Proton-GE、Wine-GE 以及类似的工具
  - 带受管前缀与内置 DXVK 2.7.1 的系统 Wine
- 在可用时用 `VK_NV_optical_flow` 生成合成运动矢量。
- HDR 输入：在 HDR 交换链上，模型看到的不是色调映射后的 8 位副本，而是该帧真实光线的 float16 代理 ——
  当交换链携带 PQ 时先做 PQ 解码。
- Qt GUI，用于实时控制：
  - 启用/禁用神经处理
  - 多 Pass 渲染
  - DLSS5 Native/Natural/Cinematic 预设
  - 强度、色调、结构、皮肤结构与锐度
  - 逐 Pass 覆盖
  - 合成运动矢量的启用、缩放模式与质量
  - 导入 NVIDIA NGX DLL（齿轮菜单 -> **NGX binaries**）
- CLI 助手管理器：
  - 运行器发现
  - start/stop/status
  - 诊断
  - NVIDIA NGX 二进制导入
- 感知 XDG 的路径：
  - 配置：`$XDG_CONFIG_HOME/dlssnr/config.ini`
  - 运行时/SHM/PID：`/tmp/dlssnr-$UID/`（不是 `$XDG_RUNTIME_DIR`，Steam 的容器会把它变成私有）
  - 日志/状态：`$XDG_STATE_HOME/dlssnr/`，回退到 `~/.local/state/dlssnr/`
  - 受管前缀：`~/.local/share/dlssnr/prefix/`

## 环境要求

- NVIDIA GPU 与 NVIDIA 驱动。
- Vulkan 加载器。
- GUI 需要 Qt 6。
- 以下之一：
  - 捆绑了 DXVK-NVAPI 的自定义 Steam 兼容性工具，例如 Proton-CachyOS 或 Proton-GE
  - 用于回退路径的系统 Wine

Valve 官方的 Proton 发布版本与 Proton Experimental 不作为主要运行器，因为它们没有捆绑所需的 DXVK-NVAPI 组件栈。系统 Wine 可以自动获取开源的运行时 DLL。

## Wine 运行时 DLL

系统 Wine 路径需要来自 DXVK 的 `dxgi.dll` 和来自 DXVK-NVAPI 的 `nvapi64.dll`。助手按以下顺序解析它们：

1. 所配置的二进制文件夹中已存在的文件（默认是 `$XDG_DATA_HOME/dlssnr/binaries`）。
2. 已安装的兼容 Proton 工具。
3. 从官方 DXVK 与 DXVK-NVAPI GitHub 项目下载的固定版本发布包，并做 SHA256 校验。

下载的文件会缓存在 `$XDG_DATA_HOME/dlssnr/binaries`，它们不是 NVIDIA NGX 文件。设置 `DLSSNR_AUTO_DOWNLOAD=0` 可要求用户手动提供这些文件。这些组件适用上游项目各自的许可证；本项目不会在自己的软件包中再分发它们。

## NVIDIA NGX DLL

公开软件包不包含 NVIDIA 专有的 NGX DLL。

只有 `nvngx_dlssnr.dll` 是必需的 —— 缺少它神经处理就会保持关闭。其余文件都是可选的，
取决于运行器：

- `nvngx_dlssnr.dll` —— 必需（模型本体）
- `nvngx.dll`、`nvapi64.dll` —— 供 core/NVAPI 路径使用；在 Proton 下可选，那里 DXVK-NVAPI
  已经提供了 NVAPI（助手在这种情形下会跳过内置的 `nvapi64.dll`）
- `sl.*.dll` —— Streamline DLL；本次构建不会加载它们

用 CLI 导入它们：

```bash
dlssnr-helper import-binaries /path/to/dlls
```

或者从 GUI 操作：齿轮菜单 -> **NGX binaries** -> **Import binaries...**。首次启动时，如果
缺少 `nvngx_dlssnr.dll`，GUI 也会提示导入。两种方式都会把文件复制到
`$XDG_DATA_HOME/dlssnr/binaries`；之后重启助手即可加载它们。

个人版软件包包含这些 DLL。只有在你有权这样做时才可再分发个人版。

## 从 RPM 安装

RPM 是 `dist/` 下的打包构建产物，由[打包](#打包)生成。请用
`dnf` 安装，这样依赖（Qt 6、Vulkan 加载器）会一并装上：

公开软件包：

```bash
sudo dnf install ./dist/dlssnr-[0-9]*.x86_64.rpm
```

个人版软件包：

```bash
sudo dnf install ./dist/dlssnr-personal-*.x86_64.rpm
```

`wine` 是推荐软件包而非硬性依赖，因此只用 Proton 的用户不会被迫安装宿主 Wine。

安装时还会为每个用户写入 `~/.config/environment.d/dlssnr.conf` —— 这是一段 systemd 用户会话
片段，其中固定 `VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"`，以便本层与
Smooth Motion 一起排列在正确的顺序上。它在下次登录时生效，绝不会覆盖已存在的
文件，并且在卸载时（如果你没有编辑过它）会被移除。

### 更新 RPM 安装

如果助手正在运行，先停止它，然后原地升级 —— 用户配置、状态与受管
前缀都会在更新中保留：

```bash
dlssnr-helper stop
sudo dnf upgrade ./dist/dlssnr-[0-9]*.x86_64.rpm
```

（在没有 `dnf` 的系统上，`rpm -Uvh ./dist/dlssnr-[0-9]*.x86_64.rpm` 可以完成同样的工作。）
重新启动任何曾通过该层呈现的游戏，以便它加载新的层库。

这两个变体携带相同的文件并且彼此冲突，因此在它们之间切换是一次
替换（swap），而不是一次安装：

```bash
sudo dnf swap dlssnr dlssnr-personal      # or the other way round
```

## 从 tarball 安装

tarball 可在任何 x86_64 glibc 发行版上运行 —— Fedora、openSUSE、Debian 及其衍生版，以及 Arch 和
CachyOS。自 `0.2.5-2` 起，GUI 二进制通过 GOT 访问 Qt 的元对象数据符号，因此它
既能对默认可见性的 Qt（Fedora）加载，也能对受保护可见性的 Qt（Arch/CachyOS）加载；
较旧的 tarball 在 Arch 系系统上会在 exec 阶段因
`GNU_PROPERTY_1_NEEDED_INDIRECT_EXTERN_ACCESS` 而失败。

解压 tarball（`*` 匹配你刚构建的任意版本；个人版请使用
`dlssnr-personal-*` tarball）：

```bash
tar -xzf dist/dlssnr-[0-9]*-linux-x86_64.tar.gz
cd dlssnr-*-linux-x86_64
```

用户安装，无需 root：

```bash
./install.sh --user
```

系统安装：

```bash
sudo ./install.sh --system
```

对于用户安装，请确保 `~/.local/bin` 在你的 `PATH` 中。

`install.sh` 还会写入 `~/.config/environment.d/dlssnr.conf`（系统安装为
`/etc/environment.d/dlssnr.conf`），其中固定 `VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"`，
以便本层与 Smooth Motion 一起排列在正确的顺序上。它在下次登录时生效。

### 更新 tarball 安装

`install.sh` 会原地覆盖它自己管理的文件，因此更新只是把新 tarball 安装
到旧版本之上 —— 用户配置、状态与受管前缀都不会被动到：

```bash
dlssnr-helper stop
tar -xzf dist/dlssnr-[0-9]*-linux-x86_64.tar.gz
cd dlssnr-*-linux-x86_64
./install.sh --user        # or: sudo ./install.sh --system
```

重新启动任何曾通过该层呈现的游戏，以便它加载新的层库。

请坚持使用一种安装模式与一种打包格式。`--user` 与 `--system` 目录树、以及 tarball
与 RPM 都拥有同一份层清单，而加载器会读取它先找到的那一份 —— 如果你
切换过模式或原本来自 RPM，请先移除另一份副本（`sudo ./uninstall.sh --system`，或
`sudo dnf remove dlssnr`）。

## 首次运行

初始化配置：

```bash
dlssnr-helper init
dlssnr-helper doctor
```

启动助手：

```bash
dlssnr-helper start
```

检查状态：

```bash
dlssnr-helper status
```

停止助手：

```bash
dlssnr-helper stop
```

GUI：

```bash
dlssnr-gui
```

## 游戏中的用法

在启用该层的情况下启动游戏：

```bash
VKLayer_DLSS5=1 ./your_native_game
```

对于 Steam：

```text
VKLayer_DLSS5=1 %command%
```

除非存在 `VKLayer_DLSS5=1`，否则该层处于禁用状态。

该层的设计目标是与 Steam 覆盖层共存。如果游戏在创建 Vulkan 设备时崩溃，请确认你使用的是 `0.2.1-3` 或更新版本。

## Steam / Proton 容器

Steam 游戏运行在 Steam Linux Runtime / `pressure-vessel` 容器内，该容器给游戏一个
位于 `$XDG_RUNTIME_DIR` 的**私有** `tmpfs`。助手放在那里的任何东西在
游戏内都不存在，于是层会在一个在日志里读起来一模一样的路径上创建自己那份空映射，
然后永远等待一个其实正在另一个文件上应答的助手：

```text
[dlssnr-layer] [shm] attached /run/user/1000/dlssnr/shm.bin seq_req=0 seq_resp=0
[dlssnr-layer] [shm] no answer in 20 ms x4 (helper not running); passing frames through
```

因此，映射位于 **`/tmp/dlssnr-$UID/`**，容器会从宿主
绑定挂载该目录。为此不需要在启动选项里添加任何东西；`VKLayer_DLSS5=1 %command%` 就够了。

如果你在较旧的构建上遇到了上面的症状，请移除它遗留的映射：

```bash
rm -rf "$XDG_RUNTIME_DIR/dlssnr"
```

如果残留仍然存在，`dlssnr-helper doctor` 会报告它。

### 自定义 `DLSSNR_SHM` 路径

如果你把 `DLSSNR_SHM` 指向别处，那个目录也必须能在容器内
可见。要么把它放在 `/tmp` 下，要么把它暴露出来：

```text
PRESSURE_VESSEL_FILESYSTEMS_RW=/home/USERNAME/.local/share/dlssnr VKLayer_DLSS5=1 %command%
```

## 运行器发现

自定义兼容性工具的发现位置：

```text
$XDG_DATA_HOME/Steam/compatibilitytools.d
~/.var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d
~/snap/steam/common/.local/share/Steam/compatibilitytools.d
$XDG_DATA_DIRS/steam/compatibilitytools.d (each entry, plus /usr/local/share and /usr/share always)
```

系统级工具（CachyOS 在 `/usr/share/steam/compatibilitytools.d` 下提供了 Proton-CachyOS）会
从 `$XDG_DATA_DIRS` 中的每个目录发现，并且由于 Steam Runtime 会在其容器内
重写该变量，还会无条件地从 XDG 默认值 `/usr/local/share` 和 `/usr/share` 中发现。
用户目录会先被扫描，因此在与系统副本打平时，用户安装的工具获胜。

列出发现的运行器：

```bash
dlssnr-helper runners
```

你也可以在下面手动设置自定义运行器路径：

```text
~/.config/dlssnr/config.ini
```

示例：

```ini
runner_type=proton
runner_path=/path/to/compatibilitytools.d/Proton-CachyOS/proton
```

## 诊断

常用命令：

```bash
dlssnr-helper doctor
dlssnr-helper runners
dlssnr-helper status
dlssnr-helper config
```

如果你以前用过较旧的构建，请移除过期的共享内存文件：

```bash
rm -f /tmp/dlssnr_shm.bin
rm -rf "${XDG_RUNTIME_DIR:-/nonexistent}/dlssnr"
```

当前构建默认使用 `/tmp/dlssnr-$UID/shm.bin`，因此游戏和助手都不需要手动设置 `DLSSNR_SHM` —— 在 Steam 容器内也是如此。

日志会写入 XDG 状态目录：

```text
$XDG_STATE_HOME/dlssnr/helper.log
```

回退位置：

```text
~/.local/state/dlssnr/helper.log
```

## 从源码构建

安装构建依赖：

- `clang`、`clang++`、`llvm-ar` 和 `llvm-strip`
- `meson` 和 `ninja`
- `mingw64-gcc-c++`（供 Clang Windows 目标使用的 MinGW 头文件、库与运行时 sysroot）
- GUI 所需的 `qt6-qtbase-devel`
- 如果需要 RPM，则需 `rpm-build`
- `glibc-devel.i686`、`libstdc++-devel.i686`、`libgcc.i686` 和 `libatomic.i686`，用于
  必需的 32 位层

Vulkan 头文件已内置，因此不需要 Vulkan 开发包。

用 Clang 构建全部内容：

```bash
tools/meson-build.sh
```

它会协调原生 64 位 Linux、32 位 Linux 以及 Windows GNU PE 目标。Windows
二进制旨在运行于 Wine 或 Proton 下，不需要原生 Windows SDK 或
编译器。构建没有副作用；请安装软件包，或使用打包脚本来安装
Vulkan 层清单。Linux 命令行工具是完全静态的，而层/GUI 在受支持的情况下
会内嵌各自的 C++ 运行时。Vulkan 加载器、Qt、图形库以及系统 C 库仍然
是动态的系统依赖。

一步完成构建与打包：

```bash
./packaging/make-dist.sh tar     # + the .tar.gz tarballs (public + personal)
./packaging/make-dist.sh rpm     # + RPMs (public + personal)
./packaging/make-dist.sh deb     # + DEBs (public + personal)
./packaging/make-dist.sh         # + tarballs and RPMs
./packaging/make-dist.sh all     # + tarballs, RPMs, and DEBs
```

输出：

```text
build/native/layer_linux/libVkLayer_NV_dlssnr.so  64-bit layer
build/linux32/layer_linux/libVkLayer_NV_dlssnr.so 32-bit layer
build/windows/windows/dlssnr_helper.exe           Windows NGX helper
build/windows/windows/smoke.exe                   smoke-test program
build/native/tools/runner_probe                   runner discovery probe
build/native/tools/dlssnr-shmctl                  shared-memory settings CLI
build/native/gui/dlssnr_gui                       Qt GUI
build/native/test_gui/binder_test                 GUI binder regression test
```

## GUI 设置

启动 GUI：

```bash
dlssnr-gui
```

每个设置都是滚动列表中的一行，并按它作用的对象分组。模型自身的配置档位于
**Model** 下，名为 `Style`：

```text
Default     maps to DLSSNR.Style 0
Natural     maps to DLSSNR.Style 1
Cinematic   maps to DLSSNR.Style 2
```

运动矢量位于 **Motion** 下：

```text
Estimate motion vectors   on by default
Motion quality            Fast, Balanced (default), Quality
Motion units              Normalised, Pixels (default), UV 0..1
```

Passes 对话框一次覆盖一个 Pass 的设置，逐字段进行：每一行都有自己的
复选框，未勾选的字段会沿用全局值，而不是重新声明它。

设置会写入共享内存，并在下一个被处理的帧上生效。关闭 GUI 会请求停止助手，因此无需手动停止助手。

齿轮菜单（右下角）中还有 **NGX binaries**：把 NVIDIA DLL 导入
`$XDG_DATA_HOME/dlssnr/binaries`，或打开该文件夹自行放置。首次启动时，如果
`nvngx_dlssnr.dll` 不在那里，GUI 会提示导入它 —— 否则助手只能报告
“no NGX binaries”，游戏则继续呈现自己的帧。

## 合成运动矢量

助手可以在 `Frame[N-1]` 与 `Frame[N]` 之间生成屏幕空间运动矢量，并在调用 Feature 18 之前把它们绑定到 `DLSSNR.MVec`。

当前行为：

- `DLSSNR.Depth` 保持为 `nullptr`。
- `DLSSNR.UseAutoMask` 被设为 `1`。
- `DLSSNR.Reset` 会在第一帧以及 CPU 检测到的简单场景切换时置位。
- 可用时使用 NVIDIA 光流（`VK_NV_optical_flow`）。
- 助手把光流输入图像保留在 VRAM 中，并在 `VULKAN_EvaluateFeature(18)` 之前运行光流 Pass。
- `DLSSNR.MVec` 被填充为以源像素为单位的 `R16G16_SFLOAT` 半精度浮点矢量，默认方向为当前帧到前一帧。
- 光流输出完全在 GPU 上由一个计算 Pass 完成解码、死区过滤与放大（每种光流格式一个着色器变体：`R16G16_SFIXED5_NV` 或 `R16G16_SFLOAT`）。如果该 Pass 无法构建，则禁用估计的运动矢量，而不是在 CPU 上转换。
- 运动矢量质量控制 NVOF 的性能等级与输出网格：Fast 偏好较小的网格，Balanced 使用中等网格，Quality 偏好全分辨率光流。

环境变量控制项：

```text
DLSSNR_MVEC=0                 disable synthetic motion vectors
DLSSNR_MVEC_DIRECTION=0       use previous-to-current flow direction instead of current-to-previous
DLSSNR_MVEC_COMPUTE=0         disable the GPU deadzone pass (disables estimated motion vectors)
DLSSNR_MVEC_DEBUG=1           log first few flow/MVec statistics
DLSSNR_SCENE_CUT=0            disable scene-cut reset detection
DLSSNR_SCENE_CUT_THRESHOLD=55 mean luma difference threshold
```

传输控制项（层与助手都会读取）：

```text
DLSSNR_DMABUF=0               disable the dma-buf zero-copy transport (host copies instead)
```

构建：

```bash
tools/meson-build.sh
```

测试参数传入：

```bash
DLSSNR_HELPER_EXE="$PWD/build/windows/windows/dlssnr_helper.exe" DLSSNR_VERBOSE=1 DLSSNR_TIME=1 ./dlssnr-helper start
WINEPREFIX="$HOME/.local/share/dlssnr/prefix/pfx" \
PROTON_ENABLE_NVAPI=1 \
STEAM_COMPAT_DATA_PATH="$HOME/.local/share/dlssnr/prefix" \
VKLayer_DLSS5=1 \
DLSSNR_SMOKE_FRAMES=5 \
"$HOME/.local/share/Steam/compatibilitytools.d/Proton-CachyOS Latest/proton" run "$PWD/build/windows/windows/smoke.exe"
```

预期的助手日志行：

```text
[mvec] NV optical flow enabled ... gpu_convert=1 dir=1 xfer=1
[params] evaluate contract set: ok ... UseAutoMask=1 ... depth=null
[params] MVecScaleX=1.000000 MVecScaleY=1.000000
[mvec] first optical-flow pass completed
[time] passes=1 ... flow=... eval=... total=... ms
```

如果光流不可用，助手会回退到零运动矢量并继续运行，而不会禁用神经处理。

## HDR 输入

以前，以 HDR 呈现的游戏会以 8 位色调映射副本的形式展示给模型：模型本就为评判高光而存在，
而这些高光在它看到之前就被压缩没了。开启 HDR 路径后，
代理以 `R16G16B16A16_SFLOAT` 传递，携带按白点归一化的线性光，而
模型 —— 被要求将自身创建为 HDR —— 处理的是该帧的真实动态范围。

检测是自动的。浮点交换链（`R16G16B16A16_SFLOAT`）就是线性光；色彩空间为 HDR10/PQ 的 10 位
交换链携带的是 ST 2084 编码，它在输入时被解码为线性尼特（nits），
在输出时再被重新编码。处于 SDR 色彩空间的 10 位交换链只是在已成画面上有更高精度，
仍然走 8 位路径。

GUI 中的 **HDR input** 行（Composition 标签页，Color 分组）决定这一切怎么发生：

```text
Auto              HDR when the swapchain is HDR (default)
Off               8-bit proxy whatever the game presents
Force float16     float proxy even for an SDR swapchain -- an A/B tool, not a preference
```

`dlssnr-shmctl <shm> set hdrmode 0|1|2` 可以通过快捷方式完成同样的事。

有三样东西对层的检测拥有最终决定权，顺序如下：设备（它必须
能够承载 float16 存储表面）、模型（会以 HDR 向其申请特性契约，如果它
拒绝，助手会自行把一切重建为 8 位并在日志中说明）、以及帧
（它绝不会以错误的位宽被读取 —— 那个切换格式的帧会按游戏
绘制它的样子呈现一帧，而不是被误读）。GUI 状态行会显示检测到了什么、以及
当前生效的是哪个代理。

白点在两条路径上含义相同 —— 合成使用的单位中 1.0 即
纸白 —— 但在 PQ 帧上，它对固定的 203 尼特参考值起乘数作用，而不是
去除一个未知的游戏尺度。实测白点在 HDR 帧上同样有效：测光表测的是
光，而不是编码值。

## 打包

所有产物都会落到 `dist/`。tarball 从协调好的 Meson 构建目录中暂存
（若产物缺失，运行 `make-dist.sh` 会先进行构建），而 RPM 以暂存的
tarball 作为其负载来安装，因此构建 RPM 也会留下 tar.gz。

```bash
./packaging/make-dist.sh          # tarballs + RPMs
./packaging/make-dist.sh tar      # tarballs only
./packaging/make-dist.sh rpm      # RPMs only
./packaging/make-dist.sh deb      # DEBs only
./packaging/make-dist.sh all      # tarballs + RPMs + DEBs
```

两个变体总会被暂存：`dlssnr`（公开版，不含 NVIDIA DLL）与 `dlssnr-personal`（捆绑
`binaries/` 中的 DLL）。公开软件包不包含 NVIDIA DLL。个人版软件包包含 ——
只有在你有权这样做时才可再分发它。

产物：

```text
dist/dlssnr-<version>-<release>-linux-x86_64.tar.gz
dist/dlssnr-personal-<version>-<release>-linux-x86_64.tar.gz
dist/dlssnr-<version>-<release>.fcXX.x86_64.rpm
dist/dlssnr-personal-<version>-<release>.fcXX.x86_64.rpm
```

### 制作一次发布

1. 提升两个 spec 中的 `Version:`，以及 `packaging/make-dist.sh` 里 `DLSSNR_VERSION` 的默认值。
2. 同时提升**两个** spec 中的 `%global pkg_release`。tarball 名称是从
   `dlssnr.spec` 读取的，而两个 spec 解包的是同一个 tarball，因此两个 release 必须一致。
   对于一次性构建，可以用 `DLSSNR_VERSION` / `DLSSNR_RELEASE` 环境变量覆盖。
3. 为两个 spec 添加 `%changelog` 条目。
4. 构建全部内容并打包：

   ```bash
   ./packaging/make-dist.sh
   ```

   Meson 会跟踪编译器与链接器的变化并重建受影响的构建目标，因此陈旧的 GCC 构建
   产物无法混进 tarball。

## 卸载

RPM：

```bash
sudo dnf remove dlssnr
```

或：

```bash
sudo dnf remove dlssnr-personal
```

tarball 安装 —— 脚本随 tarball 一起分发，因此请从解压出的目录树中运行它们
（也就是你当时安装所用的同一棵目录树；更新版本的目录树同样可以正常移除这些文件）：

```bash
./uninstall.sh --user
```

tarball 系统安装：

```bash
sudo ./uninstall.sh --system
```

两种模式都会移除二进制、桌面条目、按架构区分的层清单
（`VK_LAYER_NV_dlssnr.*.json`）以及 `environment.d/dlssnr.conf` 层顺序片段，从而让加载器
不再查找该层。加上 `--purge` 还会额外移除用户配置、状态、运行时数据与
受管前缀。RPM 也会移除该片段，但仅当它仍然与软件包
写入的内容完全一致时才这么做。

## 故障排查

如果助手启动后立刻记录 `shutting down`，请更新到 `0.2.1-3` 或更新版本，并移除过期的运行时状态：

```bash
rm -f "/tmp/dlssnr-$UID/shm.bin"
```

如果某个 Steam 游戏在 `steamoverlayvulkanlayer.so` 中崩溃，请更新到 `0.2.1-3` 或更新版本。

如果某个 Steam/Proton 游戏在助手正等待帧的时候记录 `no answer ... (helper not running)`，
说明它和助手处于不同的映射上 —— 参见 [Steam / Proton
容器](#steam--proton-容器)。

如果你在本层之外还运行 Smooth Motion，这两个隐式层可能会以错误的顺序
排列，帧就会被错误地叠放。安装程序通过
`~/.config/environment.d/dlssnr.conf` 中的 `VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"`
固定顺序（参见[从 tarball 安装](#从-tarball-安装)）；如果你是在 `0.2.5-3` 之前安装的，请自行添加该
文件，然后注销并重新登录。

## 重要说明

- 助手与游戏运行在各自独立的进程中。在驱动允许的情况下，帧以
  dma-buf 内存（`VK_EXT_external_memory_dma_buf`）跨越进程边界，并全程留在 VRAM 中；
  `DLSSNR_DMABUF=0` 会关闭这一行为。助手会导出跨越边界的这两张图像，
  并在共享头中为它们命名；层则用 `pidfd_getfd` 接管每个描述符
  （在较旧的内核上回退到 `/proc/<pid>/fd`），这需要相同的 uid 以及宽松的 yama
  设置（`ptrace_scope` 为 0）。任何无法满足的步骤 —— 扩展不可用、导出被拒绝、
  描述符接管被阻止 —— 都会回退到
  宿主传输路径，再回退到普通的暂存拷贝，因此帧总能以某种方式跨越过去。
- 该层目前假定了一种在已测试的游戏和模拟器上可用的呈现时交换链布局。部分游戏可能需要额外的布局处理工作。
- Steam 运行时的问题可能需要针对单个游戏或单个运行时进行调试。
- 除非你确定可以再分发捆绑的 NVIDIA 二进制，否则不要公开使用个人版软件包。
