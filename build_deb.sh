#!/bin/bash
# 在 UOS 20 / 飞腾 D3000 (aarch64, glibc 2.28) 上重建 LocalSend deb
# 用法: bash build_deb.sh <新版 librust_lib_localsend_app.so> <输出 deb 路径>
set -euo pipefail

BUILD=/home/uos/localsend-build
NEW_RUST_SO="${1:?需要新版 librust_lib_localsend_app.so}"
OUT="${2:-$BUILD/LocalSend-1.18.2-linux-arm-64-uos-glibc228.deb}"
TOLERANT_PLUGIN="${3:-}"          # 可选：兼容增强版 libdesktop_drop_plugin.so
VERSION="${VERSION:-1.18.2+64.uos2}"
PY=/home/uos/.workbuddy/binaries/python/versions/3.13.12/bin/python3
# 本机 /tmp 为 10MB tmpfs，dpkg-deb 压缩需要大临时空间
export TMPDIR="$BUILD/tmp"
mkdir -p "$TMPDIR"

cd "$BUILD"
mkdir -p pkg
cp -a root/. pkg/          # 幂等覆盖，不做整体删除
mkdir -p pkg/DEBIAN

# 1) 用本机重新编译的 Rust 核心替换官方版本
install -m 644 "$NEW_RUST_SO" pkg/opt/localsend_app/lib/librust_lib_localsend_app.so

# 2) 启动器二进制只差 __libc_start_main 的版本标签（2.34 -> 2.17）
$PY patch_versym.py pkg/opt/localsend_app/localsend_app __libc_start_main GLIBC_2.17

# 2.5) 国产 GPU（景嘉微/兆芯集成/Innosilicon mwv207 等）没有 Flutter 可用的 EGL，
#      默认黑屏。把真实二进制改名为 .real，用 wrapper 强制软件渲染。
rm -f pkg/opt/localsend_app/localsend_app.real
mv pkg/opt/localsend_app/localsend_app pkg/opt/localsend_app/localsend_app.real
cat > pkg/opt/localsend_app/localsend_app <<'EOF'
#!/bin/sh
# UOS + 国产 GPU（景嘉微/Innosilicon mwv207 等，无可用 EGL）下
# Flutter 默认 OpenGL 渲染会黑屏，这里强制软件渲染。
# 如需恢复硬件渲染：export FLUTTER_LINUX_RENDERER=opengl
: "${FLUTTER_LINUX_RENDERER:=software}"
export FLUTTER_LINUX_RENDERER
SELF=$(readlink -f "$0" 2>/dev/null || echo "$0")
exec "$(dirname "$SELF")/localsend_app.real" "$@"
EOF
chmod 755 pkg/opt/localsend_app/localsend_app

# 2.6) 拖拽兼容增强插件（可选）：接受 text/plain 等非标准来源
if [ -n "$TOLERANT_PLUGIN" ]; then
  install -m 644 "$TOLERANT_PLUGIN" pkg/opt/localsend_app/lib/libdesktop_drop_plugin.so
fi

# 3) 控制文件：把 libc6 的地板降到 2.28
SIZE=$(du -sk pkg | cut -f1)
sed -e "s/^Pre-Depends:.*/Pre-Depends: libc6 (>= 2.28)/" \
    -e "s/^Installed-Size:.*/Installed-Size: $SIZE/" \
    -e "s/^Version:.*/Version: $VERSION/" \
    DEBIAN_orig/control > pkg/DEBIAN/control

cp DEBIAN_orig/postinst DEBIAN_orig/postrm pkg/DEBIAN/
chmod 755 pkg/DEBIAN/postinst pkg/DEBIAN/postrm
chmod 644 pkg/DEBIAN/control

# 4) 重新打包
dpkg-deb --build --root-owner-group -Zxz pkg "$OUT"
echo "== 产物 =="
ls -lh "$OUT"
