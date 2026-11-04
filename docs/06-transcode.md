# 转码链路踩坑实录：LAME、帧长与 muxer

> v0.1.1 留下的现场记录。给以后写文章、给别人造轮子、也给自己别再踩第二遍。

## 1. LAME 3.100 交叉编译：libtool 在 Windows 上基本别指望

目标是把 `libmp3lame` 编成 ohos 的动态库，给 FFmpeg 的 `--enable-libmp3lame` 用。前前后后踩了四个坑：

### 坑 1：config.sub 不认识 `-ohos`

LAME 3.100 自带的 `config.sub` 是 2010 年代的，`--host=aarch64-linux-ohos` 直接报
`invalid configuration`。

解法：`--host` 用 `aarch64-unknown-linux-gnu`，真正的编译器通过 wrapper 指定：

```sh
exec ".../clang.exe" --target=aarch64-linux-ohos --sysroot=".../sysroot" "$@"
```

`--host` 只影响 libtool 的命名，不影响产物 ABI。

### 坑 2：libtool 找不到 `ld`

```
checking for ld used by cc... no
configure: error: no acceptable ld found in $PATH
```

MSYS2 里没装 binutils（`usr/bin/ld.exe` 不存在），而 `clang -print-prog-name=ld`
返回的是 `C:/mingw64/bin/ld.exe`——同样不存在。

解法：显式 `LD="<NDK>/llvm/bin/ld.lld.exe"`，同时 `--with-gnu-ld` 跳过 GNU ld 探测
（lld 的 version 串是 `LLD ... (compatible with GNU linkers)`，老 libtool 不认）。

### 坑 3：libtool 的共享库链接在 Windows 上静默失败

configure 过了、编译也过了，卡在：

```
libtool: link: (cd ".libs" && rm -f "libmp3lame.so.0" && cp -pR "libmp3lame.so.0.0.0" "libmp3lame.so.0")
cp: cannot stat 'libmp3lame.so.0.0.0': No such file or directory
```

实际链接命令根本没执行（日志里连 `libtool: link: cc -shared ...` 都没有），
这是 libtool 在 msys 环境下拼路径的老毛病，不值得修。

解法：**绕开 libtool 的共享库模式**——`--enable-static --disable-shared` 只生成
`libmp3lame.a`，然后自己用 clang 链：

```sh
"$tdir/cc" -shared \
  -Wl,-soname,libmp3lame.so.0 \
  -Wl,-z,max-page-size=16384 \
  -o libmp3lame.so.0.0.0 \
  -Wl,--whole-archive libmp3lame/.libs/libmp3lame.a -Wl,--no-whole-archive \
  -lm
```

`--whole-archive` 是必须的：没有任何东西在链接期引用 lame 的符号，不加的话
生成的 .so 是空的。

### 坑 4：重复链接 mpglib 会 duplicate symbol

libtool 已经把 `mpglib` 的目标文件并进了 `libmp3lame.a`，再单独链
`mpglib/.libs/libmpgdecoder.a` 就报：

```
ld.lld: error: duplicate symbol: decode_layer3_sideinfo
```

所以只链 `libmp3lame.a` 一个归档。

另外 `CFLAGS` 必须带 `-fPIC`，否则静态对象进不了 `.so`。

最终产物：`libmp3lame.so.0`，arm64 **336,808 B** / x86_64 **377,360 B**，
SONAME `libmp3lame.so.0`，只依赖 libc。作为发布包的第 5 个 `.so`。

## 2. 三种编码器的帧长完全不一样

这是最隐蔽的一类 bug：`avcodec_send_frame` 返回 `-22 (Invalid argument)`，
但 avctx 的采样率、声道、格式全对，错的是**每帧采样数**。

| 编码器 | frame_size | 采样率要求 | 备注 |
| --- | --- | --- | --- |
| `aac`（原生） | 1024 | 任意常见 | — |
| `libmp3lame` | 1152 | / | LGPL 外部库 |
| `opus`（原生） | 960（20 ms @48k） | **只支持 48 kHz** | 带 `AV_CODEC_CAP_EXPERIMENTAL` |

踩坑经过：44.1 kHz 的 AAC 解码一帧是 1024 个采样，swr 升到 48 kHz 输出
**1115** 个采样，直接 `send_frame` 就 EINVAL：

```
[transcode] send_frame ret=-22 (Invalid argument) nb=1115 fmt=8 pts=0
```

正确姿势：

1. `enc->frame_size` 在 `avcodec_open2` 之后才知道，不要写死；
2. 用一块 `staging` 帧（大小 = frame_size）做累积，swr 每吐一块就按剩余空间切片；
3. `pts` 按**累计输出采样数**递增，不是按 swr 调用次数；
4. 最后一帧不足 frame_size 时直接送——AAC/LAME/Opus 都带 `AV_CODEC_CAP_SMALL_LAST_FRAME`；
5. 送完一帧 `av_frame_make_writable()`，防止编码器持有引用时下一轮覆盖。

## 3. FFmpeg n9.0 的 API 变化

- **`AVCodec::sample_fmts` 没了**。以前直接读 `codec->sample_fmts[0]`，现在必须查：

  ```c
  const void *cfgs = NULL; int n = 0;
  avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfgs, &n);
  fmt = ((const enum AVSampleFormat *)cfgs)[0];
  ```

- **Opus 编码器是实验性的**：`avcodec_open2` 前必须
  `avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL`，否则直接拒绝。

- **muxer 对应关系**：AAC → `mp4`（或 `ipod`，M4A brand 更规范）；
  MP3 → `mp3`；Opus → `opus`（Ogg 封装）。

## 4. 一个和转码无关、但更致命的坑

`check-release.py` 校验的是 `prebuilt/` 和 `dist/` 里的 zip，**不校验 HAP**。
而 HAP 打包用的是 `harmony/entry/libs/<abi>/` 这份独立拷贝。prebuilt 重编后如果
忘了同步，HAP 里还是旧 `.so`，运行时才报 `alloc_output_context2(ipod) failed`。

现在 `build/hvigor-build.sh` 每次打包前自动跑 `build/install-into-hap.sh`，这个坑封死了。

## 5. 复现命令

```bash
# 1) LAME（libmp3lame.so.0）
bash build/build-lame.sh all

# 2) FFmpeg（链接 lame，开 opus/ogg）
bash build/build-ffmpeg.sh all

# 3) 组件/包/README 一致性校验
python build/check-release.py

# 4) 打包（内含 check-release，失败会删产物）
python build/make-release.py
```
