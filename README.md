# LocalSend for UOS 飞腾 D3000

> **本项目不是 LocalSend 官方发行版。**
> 它是在官方 [localsend/localsend](https://github.com/localsend/localsend) **v1.18.2** 源码基础上，
> 针对 **UOS 20（深度/deepin 系）+ 飞腾 D3000（aarch64）+ glibc 2.28** 做的重新构建与适配。
> 上游代码版权归 LocalSend 原作者所有，本项目仅提供重新打包脚本与二进制安装包。

## 为什么需要它

官方 `LocalSend-1.18.2-linux-arm-64.deb` 声明 `Pre-Depends: libc6 (>= 2.31)`，而 UOS 20 的
`libc6:arm64` 版本是 `2.28.36-deepin1`，直接 `dpkg -i` 会被依赖挡住。

但**只改依赖声明是不够的**——包里 17 个 ELF 逐个核对后，真正需要高版本 glibc 的只有两个：

| 组件 | 实际需求 | 本项目的处理 |
| --- | --- | --- |
| `librust_lib_localsend_app.so`（Rust 核心） | `log/pow@2.29`、`gettid@2.30`、`stat64/fstat64@2.33`、`pthread_*@2.34` | **在本机原生重新编译** |
| `localsend_app`（C++ 启动器） | 仅 `__libc_start_main@2.34` 一个符号 | **改写 ELF 版本标签** |
| `libflutter_linux_gtk.so`（Flutter 引擎） | GLIBC_2.18 | 原样复用 |
| `libapp.so`（Dart AOT 业务代码） | 不依赖 glibc | 原样复用 |
| 其余 11 个插件 `.so` | GLIBC_2.17 | 原样复用 |

装得上却一启动就崩，是这个包的典型症状，所以 Rust 核心必须真编译，不能靠改版本号蒙混。

## 做了三件事

1. **重新编译 Rust 核心**
   用 [rustup](https://rustup.rs) 装 **1.97.1**（与官方 `.so` 里 `.comment` 记录的编译器版本一致，
   保证 flutter_rust_bridge 的 ABI 对齐），在本机 `cargo build --release -p rust_lib_localsend_app`。
   本机编译产物天然只要求 `GLIBC_2.28`。

2. **修复启动器的 ELF 版本标签**（`patch_versym.py`）
   启动器只差 `__libc_start_main` 一个符号的版本标签，函数本身与 2.17 完全一致。
   坑在于：只改符号指向不够，加载器会逐条校验 `.gnu.version_r`，
   必须把残留的 `GLIBC_2.34` 依赖记录一并从链上摘掉，否则照样报
   `version GLIBC_2.34 not found`。

3. **修复国产 GPU 黑屏**（`build_deb.sh` 中的 wrapper）
   飞腾整机常见的景嘉微 JM9100 / Innosilicon mwv207 之类国产卡没有 `/dev/dri`，
   Flutter 引擎建不出 OpenGL 上下文，窗口就是一片黑。
   解决方式是注入 `FLUTTER_LINUX_RENDERER=software` 走 Skia 软件渲染：
   真实二进制改名为 `localsend_app.real`，用同名 wrapper 脚本转发，
   这样从菜单、托盘、终端或开机自启动都能生效。

## 安装

```bash
sudo dpkg -i LocalSend-1.18.2-linux-arm64-uos-glibc228-v2-for-UOS飞腾D3000.deb
```

依赖（UOS 桌面版通常已自带，无需 `apt -f install`）：
`libappindicator3-1 | libayatana-appindicator3-1`、`libayatana-ido3-0.4-0`、
`gir1.2-ayatanaappindicator3-0.1`、`xdg-user-dirs`。

装完会生成 `/usr/bin/localsend_app` 软链接和 `/usr/share/applications/localsend_app.desktop`，
应用菜单里会出现图标。版本号标为 `1.18.2+64.uos2`，与官方包区分。

## 验证结果（飞腾 D3000 / UOS 20 实测）

- 全包 ELF 最高 glibc 需求：**2.28**（与系统吻合）
- `ldd -r`：缺失符号 0、版本错误 0
- 实跑：Rust 核心在 53317 端口正常起 HTTPS 服务，`openssl s_client` 握手协商 **TLS 1.3**
- `apt-get install --simulate`：无未满足依赖
- 界面：软件渲染下完整渲染，GL 相关错误 0
- 拖拽发送：`file://` URI 拖入可正常进入发送列表

## 自己重建一份

```bash
# 1. 拿源码
curl -L -o localsend-src.tar.gz https://github.com/localsend/localsend/archive/refs/tags/v1.18.2.tar.gz
tar xzf localsend-src.tar.gz && cd localsend-1.18.2

# 2. 装 Rust 1.97.1（国内建议走 rsproxy 镜像）
export RUSTUP_DIST_SERVER=https://rsproxy.cn RUSTUP_UPDATE_ROOT=https://rsproxy.cn/rustup
rustup toolchain install 1.97.1 --profile minimal

# 3. crates 镜像（index.crates.io 在部分网络下会超时）
mkdir -p ~/.cargo && cat > ~/.cargo/config.toml <<'EOF'
[source.crates-io]
replace-with = "rsproxy-sparse"
[source.rsproxy-sparse]
registry = "sparse+https://rsproxy.cn/index/"
EOF

# 4. 编译 Rust 核心
cargo build --release -p rust_lib_localsend_app

# 5. 解包官方 deb 作为底座，然后重新打包
dpkg-deb -x LocalSend-1.18.2-linux-arm-64.deb root
dpkg-deb -e LocalSend-1.18.2-linux-arm-64.deb DEBIAN_orig
bash build_deb.sh target/release/librust_lib_localsend_app.so out.deb
```

`build_deb.sh` 里的 `BUILD` 目录默认 `/home/uos/localsend-build`，可用环境变量覆盖：

```bash
BUILD_DIR=/你的/构建目录 bash build_deb.sh <新.so> <输出deb>
```

### 两个踩过的坑

- **`/tmp` 太小**：本机 `/tmp` 只有 10MB，`dpkg-deb` 压缩会失败，脚本已把 `TMPDIR` 指向构建目录。
- **别直接 `rm -rf` 重建目录**：脚本用的是幂等覆盖（`mkdir + cp -a`），避免误删；
  强杀 `rustup` 会清空已装 toolchain，需要重装。

## 已知问题

- `lib/libdartjni.so` 仍带 `GLIBC_2.34`：它是给 Android/JVM 用的 JNI 桥（依赖 `libjvm.so`），
  桌面 Linux 下不会被加载，官方包也是如此，未做处理。
- 软件渲染靠 CPU 绘制：静态 UI 在 8 核飞腾上完全够用；若想试硬件渲染：
  `FLUTTER_LINUX_RENDERER=opengl localsend_app`（驱动更新后值得一试）。

## 许可与致谢

- 上游 LocalSend 使用 **Apache-2.0**，见 [其仓库 LICENSE](https://github.com/localsend/localsend/blob/main/app/LICENSE)。
- 本仓库中的打包脚本（`build_deb.sh`、`patch_versym.py`）同样以 Apache-2.0 发布，可自由使用与修改。
- 感谢 LocalSend 原作者 Tien Do Nam 及社区做出的优秀开源项目；
  本适配仅为在国产平台（UOS + 飞腾）上运行而做的二次构建，与官方团队无关。

## 免责声明

本项目提供的安装包按“原样”提供，不保证在任何环境下都能正常工作，
使用前请自行评估风险。若有问题，欢迎提 issue 并附上：

```bash
DISPLAY=:0 localsend_app 2>&1 | head -50
```
