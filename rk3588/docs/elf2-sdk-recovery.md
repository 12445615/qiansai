# ELF2 SDK 恢复手册

## 固定输入与校验

| 输入 | 路径/值 |
|---|---|
| Desktop update image | `/mnt/f/ELF 2开发板资料包/03-镜像/00-eMMC镜像/Desktop镜像/update.img` |
| Desktop MD5 | `2999ff852d2e89bf9078ba9e201a61b2` |
| GCC 10.3 source | `/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/gcc-arm-10.3-aarch64-toolchain` |
| GCC archive SHA-256 | `8b79c0ba3d0f83d58200c9d0b96f253bbaeab1d03e3851701d082903720c1545` |
| FFmpeg-Rockchip commit | `99d938d6ea32c31457bb68a5a2177c94d13a4854` |
| FFmpeg archive SHA-256 | `0779552be0813989764275e411a85c14d68efffca1d3e98641e3d4bfb5ac8bad` |

宿主机需要：`e2fsprogs`、`libguestfs-tools`、`qemu-utils`、`file`、`binutils`、`dpkg-dev`、`pkg-config`、`curl`、`make` 和 AArch64 工具链。建议在解包前预留至少 20GB 空间：Desktop 镜像约 7.4GB，Jammy sysroot、工具链、FFmpeg 构建目录和 staging 还会额外占用空间。

## 执行顺序

```bash
cd ~/Linux/qiansai/rk3588
./scripts/setup_elf2_sdk.sh
./scripts/build_elf2.sh --profile buildroot-check --jobs 8
./scripts/capture_rockchip_tools.sh
./scripts/setup_elf2_jammy_sdk.sh
./scripts/install_elf2_jammy_deps.sh
./scripts/build_ffmpeg6_rkmpp.sh --jobs 8
./scripts/verify_elf2_sdk.sh --profile jammy-deploy --jobs 8
```

`setup_elf2_jammy_sdk.sh --check` 只校验 Desktop MD5、解包工具和 GCC 输入，不会解包 7.4GB 镜像。真实解包必须以 WSL root 运行时确保 `guestfish` 能读取内核；该步骤只读 VM 磁盘，不会修改或删除 `ubuntu1`。

## Manifest 验证

```bash
cd ~/.local/opt/elf2-sdk/sources/rockdev-tools
sha256sum -c manifest.sha256
grep -F 'IMAGE_MD5=2999ff852d2e89bf9078ba9e201a61b2' \
  ~/.local/opt/elf2-sdk/profiles/jammy-deploy/manifests/base-inputs.sha256
grep -F 'COMMIT=99d938d6ea32c31457bb68a5a2177c94d13a4854' \
  ~/.local/opt/elf2-sdk/profiles/jammy-deploy/manifests/ffmpeg-rockchip.txt
```

## 清理边界

只清理脚本拥有且可重建的路径：`/tmp/elf2-*`、`/tmp/elf2.*` 以及 `~/.local/opt/elf2-sdk` 下的构建目录或下载缓存。不要对 `/mnt/e/VirtualBox/VMS/ubuntu1`、VM 磁盘、用户源代码或未验证的 sysroot 使用递归删除；保留 `ubuntu1` 直到板端验证清单全部有证据。
