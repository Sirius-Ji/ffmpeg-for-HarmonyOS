# ffmpeg-for-HarmonyOS

鸿蒙端（HarmonyOS NEXT / OpenHarmony）的 FFmpeg 音频解码基础库。

**当前状态：M0 完成、M1 代码与 HAP 就绪**（等签名安装看 HiLog 验收）。FFmpeg n9.0 已编出 `arm64-v8a` / `x86_64` 两套 `.so`（含 h264/hevc 鸿蒙硬解 + 音频软解白名单），NAPI 层走通"打开 → 读包 → 解码 → 写 WAV"。

## 文档

| 文件 | 内容 |
| --- | --- |
| [docs/00-recon.md](docs/00-recon.md) | 侦察报告：本机实况、生态对比、路线取舍、坑清单 |
| [docs/02-M0-build.md](docs/02-M0-build.md) | 交叉编译实录：configure 实参、5 个坑、产物校验 |
| [docs/03-M1.md](docs/03-M1.md) | M1：工程结构、NAPI 接口、预期 HiLog、验收判据 |
| [build/build-ffmpeg.sh](build/build-ffmpeg.sh) | 交叉编译（arm64-v8a / x86_64 / all） |
| [build/install-into-hap.sh](build/install-into-hap.sh) | 产物同步进 HAP 工程 |
| [build/hvigor-build.sh](build/hvigor-build.sh) | 命令行打 HAP |
| [build/verify-artifacts.sh](build/verify-artifacts.sh) | ABI / SONAME / 许可开关校验 |
| [build/audit-oh-symbols.py](build/audit-oh-symbols.py) | OH 符号 vs API 20 基线审计 |
| [test/compare.py](test/compare.py) | 设备 PCM 与桌面参考对拍 |

## 关键约定

- **基线 API 20**：代码只用到 API ≤ 11 的 OH 符号（已脚本审计），`compatibleSdkVersion` 等 API 20 SDK 装好后从 `26.0.0` 切回 `6.0.0(20)`
- **许可证**：FFmpeg 走 LGPL-2.1+，不开 `--enable-gpl` / `--enable-nonfree`；产物是动态 `.so`（LGPL §6(b)）
- **页对齐**：`.so` 的 `PT_LOAD Align = 0x4000`（16 KB）
- **解码分工**：解封装 + 软解兜底用 FFmpeg；音频主力解码留给平台 `OH_AudioCodec`（M3）；出声用 `OH_AudioRenderer`（M2）
- **打包**：`NEEDED` 里是 `libavcodec.so.63` 这种带大版本的名字，`entry/libs/<abi>/` 必须同名

## 快速开始

```bash
# 1. 交叉编译（MSYS2 的 bash，需完全权限）
bash build/build-ffmpeg.sh all

# 2. 同步进 HAP 工程 + 打包
bash build/install-into-hap.sh
bash build/hvigor-build.sh
# → harmony/entry/build/default/outputs/default/entry-default-unsigned.hap

# 3. 校验
bash build/verify-artifacts.sh
python build/audit-oh-symbols.py arm64-v8a
python test/compare.py
```

签名安装请用 DevEco 打开 `harmony/`（自动签名需要登录华为账号）。

> 注意：在 IDE 里签过名之后，`harmony/build-profile.json5` 会被写入本机证书路径和密码。提交前清回 `"signingConfigs": []`，并启用仓库自带的钩子：
> `git config core.hooksPath .githooks`
