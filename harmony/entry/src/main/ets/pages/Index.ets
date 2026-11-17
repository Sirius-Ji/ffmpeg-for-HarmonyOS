import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
// NAPI 模块：libentry.so，C 侧 nm_modname = "entry"
import { FFmpegOhos } from '@sirius/ffmpeg_ohos';
import type { NapiResult } from '@sirius/ffmpeg_ohos';

const DOMAIN = 0xFF01;
const TAG = 'FFMUI';

@Entry
@Component
struct Index {
  @State status: string = '未播放';
  @State logText: string = 'M1 自检：解 test.m4a / test.flac / test.opus 并落 WAV';
  @State busy: boolean = false;
  @State hardwareOn: boolean = true;
  @State copyPercent: number = -1;
  @State surfaceId: string = '';
  @State volume: number = 1.0;
  @State wmText: string = 'FFmpeg-ohos';
  @State wmPosition: string = 'top-left';
  @State wmColor: string = 'white';
  @State wmSize: number = 48;
  @State wmAlpha: number = 100;
  @State wmBold: boolean = false;
  @State resolution: string = '720p';
  @State convertVolume: number = 1.0;
  @State subtitlePath: string = '';
  @State fontsDir: string = '';
  private timer: number = -1;
  private videoController: XComponentController = new XComponentController();

  aboutToDisappear(): void {
    this.stopPolling();
  }

  build() {
    Scroll() {
      Column({ space: 12 }) {
      Text('ffmpeg-for-HarmonyOS · M2 播放自检')
        .fontSize(18)
        .fontWeight(FontWeight.Bold)

      // M4.1：视频渲染目标。surfaceId 在 onLoad 时拿到，交给 C++ 零拷贝解码。
      XComponent({ id: 'videoSurface', type: XComponentType.SURFACE, controller: this.videoController })
        .width('100%')
        .height(260)
        .backgroundColor(Color.Black)
        .onLoad(() => {
          this.surfaceId = this.videoController.getXComponentSurfaceId();
          this.status = `视频 Surface 就绪: ${this.surfaceId}`;
          FFmpegOhos.setVolume(this.volume);
        })

      Row({ space: 8 }) {
        Text('音量').fontSize(13)
        Slider({ value: this.volume, min: 0, max: 1, step: 0.01 })
          .width(260)
          .onChange((v: number) => {
            this.volume = v;
            FFmpegOhos.setVolume(v);
            this.status = `音量: ${(v * 100).toFixed(0)}%`;
          })
      }

      Row({ space: 12 }) {
        Button('Play（内置样例）')
          .onClick(() => {
            this.onPlay();
          })
        Button('Stop')
          .onClick(() => {
            this.onStop();
          })
      }

      Button('选歌播放（系统文件选择器）')
        .onClick(() => {
          this.pickAndPlay();
        })

      Row({ space: 8 }) {
        Button(this.busy ? '转码中…' : '转码 → AAC')
          .fontSize(13)
          .enabled(!this.busy)
          .onClick(() => {
            this.runTranscode('aac');
          })
        Button('转码 → MP3')
          .fontSize(13)
          .enabled(!this.busy)
          .onClick(() => {
            this.runTranscode('mp3');
          })
        Button('转码 → Opus')
          .fontSize(13)
          .enabled(!this.busy)
          .onClick(() => {
            this.runTranscode('opus');
          })
      }

      Row({ space: 8 }) {
        Button('转码 → FLAC')
          .fontSize(13)
          .enabled(!this.busy)
          .onClick(() => {
            this.runTranscode('flac');
          })
        Button('转码 → WAV')
          .fontSize(13)
          .enabled(!this.busy)
          .onClick(() => {
            this.runTranscode('wav');
          })
        Button('路径 API 转码')
          .fontSize(13)
          .enabled(!this.busy)
          .onClick(() => {
            this.runTranscodeByPath('aac');
          })
      }

      Button('M3.0 硬解探针（AAC/MP3/FLAC）')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => {
          this.runHardwareProbe();
        })

      Button('M4.0 视频探针（H.264/H.265）')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => {
          this.runVideoProbe();
        })

      Button('M4.2.4 Buffer 模式（FIT 留黑边）')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => {
          this.playVideo(true, false);
        })

      Button('M4.2 播放（Surface FIT）')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => {
          this.playVideo(false, false);
        })

      Button('M4.2 Surface CROP（铺满裁剪）')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => {
          this.playVideo(false, true);
        })

      Button('M4.3 视频转码 → MP4')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => {
          this.transcodeVideo();
        })

      Button('M4.4 提取纯音频')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => {
          this.extractAudio();
        })

      Button('M4.4 转 MKV')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.transcodeFormat('mkv'); })

      Button('M4.4 转 FLV')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.transcodeFormat('flv'); })

      Button('M4.4 转 HEVC')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.transcodeFormat('hevc'); })

      Button('M4.4 提取纯视频')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.extractVideo(); })

      Row({ space: 8 }) {
        TextInput({ text: this.wmText, placeholder: '水印文字' }).width(150)
          .onChange((v: string) => { this.wmText = v; })
        TextInput({ text: this.wmPosition, placeholder: 'top-left' }).width(120)
          .onChange((v: string) => { this.wmPosition = v; })
        TextInput({ text: this.wmColor, placeholder: 'white' }).width(100)
          .onChange((v: string) => { this.wmColor = v; })
        TextInput({ text: String(this.wmSize), placeholder: '48' }).width(70)
          .onChange((v: string) => { this.wmSize = parseInt(v) || 48; })
        TextInput({ text: String(this.wmAlpha), placeholder: '100' }).width(60)
          .onChange((v: string) => { this.wmAlpha = parseInt(v) || 100; })
        Toggle({ type: ToggleType.Switch, isOn: this.wmBold })
          .onChange((v: boolean) => { this.wmBold = v; })
      }

      Text('original 模式：奇数宽高自动向下取偶（853→852）')
        .fontSize(11)
        .fontColor(Color.Gray)

      Select([{ value: 'original' }, { value: '720p' }, { value: '1080p' }])
        .selected(1)
        .value(this.resolution)
        .onSelect((index: number, value: string) => { this.resolution = value; })

      Button('V0.3 组合测试')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.v03Test(); })

      Button('旋转验证')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.rotationTest(); })

      TextInput({ text: this.subtitlePath, placeholder: '字幕路径（留空用内置 subs.srt）' })
        .fontSize(12)
        .onChange((v: string) => { this.subtitlePath = v; })
      TextInput({ text: this.fontsDir, placeholder: '字体目录（留空用 filesDir）' })
        .fontSize(12)
        .onChange((v: string) => { this.fontsDir = v; })
      Button('SRT force 审计')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.subtitleTest(false, 1.0); })

      Button('ASS Style 审计')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.subtitleTest(true); })

      Button('ASS 1.0 对照')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.subtitleTest(true, 1.0); })

      Button('trim12.5 审计')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.subtitleTest(true, 1.25, 12.5); })

      Button('B 四组批量')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.auditBBatch(); })

      Button('ASS 0.5 慢放')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.subtitleTest(true, 0.5); })

      Button('层级重叠')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.layerTest(); })

      Button('降级三组')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.degradeBatch(); })

      Button('冒号/中文批量')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.specialBatch(); })

      Row({ space: 8 }) {
        Text(`音量 ${this.convertVolume.toFixed(2)}`).fontSize(12)
        Slider({ value: this.convertVolume, min: 0, max: 2, step: 0.05 })
          .width(220)
          .onChange((v: number) => { this.convertVolume = v; })
      }

      Button('视频加文字水印')
        .fontSize(13)
        .enabled(!this.busy)
        .onClick(() => { this.watermarkVideo(); })

      Button('停止视频')
        .fontSize(13)
        .onClick(() => {
          FFmpegOhos.videoStop();
          this.status = '视频已停止';
        })

      Button(this.hardwareOn ? '硬解开关：开（AAC）' : '硬解开关：关（走软解）')
        .fontSize(13)
        .onClick(() => {
          this.hardwareOn = !this.hardwareOn;
          try {
            FFmpegOhos.decoderSetHardware(this.hardwareOn);
            this.status = `硬解开关已${this.hardwareOn ? '打开' : '关闭'}，重新点 Play 生效`;
          } catch (e) {
            this.status = `切换硬解失败: ${JSON.stringify(e)}`;
          }
        })

      Text(this.status)
        .fontSize(12)
        .fontFamily('monospace')
        .width('100%')

      if (this.copyPercent >= 0) {
        Progress({ value: this.copyPercent, total: 100, type: ProgressType.Linear })
          .width('100%')
          .color('#4CAF50')
      }

      Divider()

      Button(this.busy ? '解码中…' : 'M1 解码自检')
        .enabled(!this.busy)
        .onClick(() => {
          this.runTest();
        })

      Scroll() {
        Text(this.logText)
          .fontSize(12)
          .fontFamily('monospace')
          .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
    }
    .padding(16)
    .width('100%')
    }
    .width('100%')
    .height('100%')
    .scrollBar(BarState.Auto)
  }

  // 把 rawfile 里的样例拷到应用沙箱，返回真实路径
  async ensureSample(name: string): Promise<string> {
    const ctx = getContext(this) as common.UIAbilityContext;
    const dst = `${ctx.filesDir}/${name}`;
    if (!fs.accessSync(dst)) {
      const raw = await ctx.resourceManager.getRawFileContent(name);
      const file = fs.openSync(dst, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, raw.buffer as ArrayBuffer);
      fs.closeSync(file);
    }
    return dst;
  }

  async onPlay(): Promise<void> {
    try {
      const src = await this.ensureSample('play.m4a');
      hilog.info(DOMAIN, TAG, 'onPlay: 内置样例 path=%{public}s', src);
      const openR: NapiResult = FFmpegOhos.playerOpen(src);
      if (!openR.ok) {
        this.status = `打开失败: ${openR.data}`;
        return;
      }
      const playR: NapiResult = FFmpegOhos.playerPlay();
      if (!playR.ok) {
        this.status = `播放失败: ${playR.data}`;
        return;
      }
      this.status = `内置样例 play.m4a\nplay: ${playR.data}`;
      this.startPolling();
    } catch (e) {
      this.status = `异常: ${JSON.stringify(e)}`;
      hilog.error(DOMAIN, TAG, 'onPlay failed: %{public}s', JSON.stringify(e));
    }
  }

  onStop(): void {
    try {
      this.stopPolling();
      this.status = FFmpegOhos.playerStop().data;
      hilog.info(DOMAIN, TAG, 'onStop');
    } catch (e) {
      this.status = `异常: ${JSON.stringify(e)}`;
    }
  }

  // 系统文件选择器 → URI → fd → native（不复制文件，直接读原文件）
  // 注意：这里没有任何"失败就播样例"的兜底；任何一步失败都停在那里并显示原因。
  async pickAndPlay(): Promise<void> {
    // 先清场：停掉可能正在播的会话，并停掉状态轮询，
    // 否则 500ms 一次的轮询会把错误信息刷掉，看起来像"还在播旧音频"。
    this.stopPolling();
    try {
      FFmpegOhos.playerStop();
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'playerStop before pick failed: %{public}s', JSON.stringify(e));
    }
    this.status = '正在打开文件选择器…';
    hilog.info(DOMAIN, TAG, 'pickAndPlay: start');

    let uri = '';
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const documentPicker = new picker.DocumentViewPicker(ctx);
      const options = new picker.DocumentSelectOptions();
      options.maxSelectNumber = 1;
      const uris: string[] = await documentPicker.select(options);
      hilog.info(DOMAIN, TAG, 'picker returned %{public}d uri(s)', uris.length);
      if (uris.length === 0) {
        this.status = '已取消选择：没有播放任何文件';
        return;
      }
      uri = uris[0];
    } catch (e) {
      const msg = JSON.stringify(e);
      this.status = `文件选择器失败：${msg}`;
      hilog.error(DOMAIN, TAG, 'picker failed: %{public}s', msg);
      return;
    }

    this.status = `已选择：${uri}\n正在复制到本地沙箱…`;
    hilog.info(DOMAIN, TAG, 'picked uri=%{public}s', uri);

    // 外部 URI 可能是云盘/虚拟文件，先在应用层完整拷贝到沙箱，再交给基座
    let local = '';
    try {
      local = await this.importToSandbox(uri);
    } catch (e) {
      const msg = JSON.stringify(e);
      this.status = `复制到沙箱失败：${msg}`;
      hilog.error(DOMAIN, TAG, 'copy to sandbox failed: %{public}s', msg);
      return;
    } finally {
      this.copyPercent = -1;
    }
    hilog.info(DOMAIN, TAG, 'imported local file: %{public}s', local);

    try {
      const openR: NapiResult = FFmpegOhos.playerOpen(local);
      if (!openR.ok) {
        this.status = `解码器打开失败：${openR.data}`;
        hilog.error(DOMAIN, TAG, 'playerOpen !ok: %{public}s', openR.data);
        return;
      }
      const playR: NapiResult = FFmpegOhos.playerPlay();
      hilog.info(DOMAIN, TAG, 'playerPlay -> ok=%{public}s', String(playR.ok));
      if (!playR.ok) {
        this.status = `播放启动失败：${playR.data}`;
        return;
      }
      this.status = `file: ${local}\nplay: ${playR.data}`;
      this.startPolling();
    } catch (e) {
      const msg = JSON.stringify(e);
      this.status = `播放异常：${msg}`;
      hilog.error(DOMAIN, TAG, 'playerPlay failed: %{public}s', msg);
    }
  }

  // 把 picker 选中的 URI 流式复制到应用沙箱，带进度；返回本地路径。
  // 注意：readSync/writeSync 的 options.offset 是「文件位置」而不是缓冲区偏移，
  // 所以这里不传 options，读满缓冲后 slice 再写。
  async importToSandbox(uri: string): Promise<string> {
    const ctx = getContext(this) as common.UIAbilityContext;
    const rawName = uri.split('/').pop();
    const name = (rawName === undefined || rawName.length === 0) ? 'import.bin' : rawName;
    const safe = name.replace(/[^A-Za-z0-9._-]/g, '_');
    const dst = `${ctx.filesDir}/${Date.now()}_${safe}`;
    const src = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    const out = fs.openSync(dst, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    try {
      let total = 0;
      try {
        total = fs.statSync(src.fd).size;
      } catch (e) {
        total = 0;  // 云盘/虚拟文件可能拿不到 size，用“读到 EOF”为准
      }
      const buf = new ArrayBuffer(256 * 1024);
      let done = 0;
      this.copyPercent = total > 0 ? 0 : -1;
      for (;;) {
        const n = fs.readSync(src.fd, buf);
        if (n <= 0) break;
        fs.writeSync(out.fd, buf.slice(0, n));
        done += n;
        if (total > 0) {
          this.copyPercent = Math.min(99, Math.floor(done * 100 / total));
        }
      }
      this.copyPercent = 100;
      const st = fs.statSync(dst);
      hilog.info(DOMAIN, TAG, 'copied %{public}d bytes (stat total=%{public}d), dst size=%{public}d',
                 done, total, st.size);
      if (st.size <= 0) {
        throw new Error(`复制结果为 0 字节（源 size=${total}）`);
      }
    } finally {
      fs.closeSync(src);
      fs.closeSync(out);
    }
    return dst;
  }

  // 转码示例：内置样例 play.m4a → aac/mp3/opus，完成后直接播放结果
  async runTranscode(format: string): Promise<void> {
    this.stopPolling();
    try {
      FFmpegOhos.playerStop();
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'playerStop before transcode failed: %{public}s', JSON.stringify(e));
    }
    this.busy = true;
    this.status = `转码中（${format}）…`;
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('play.m4a');
      const ext = format === 'aac' ? 'm4a' : format;
      const dst = `${ctx.filesDir}/transcoded.${ext}`;
      const inFile = fs.openSync(src, fs.OpenMode.READ_ONLY);
      const outFile = fs.openSync(dst, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      try {
        hilog.info(DOMAIN, TAG, 'transcode start %{public}s -> %{public}s (%{public}s)', src, dst, format);
        const res: NapiResult = FFmpegOhos.transcodeTo(inFile.fd, outFile.fd, format);
        hilog.info(DOMAIN, TAG, 'transcode result ok=%{public}s data=%{public}s', String(res.ok), res.data);
        if (!res.ok) {
          this.status = `转码失败（${format}）: ${res.data}`;
          return;
        }
        const st = fs.statSync(dst);
        this.status = `转码成功（${format}）: ${st.size} B\n${dst}\n${res.data}`;

        // 立刻播放转码结果，证明产物可用
        const openR: NapiResult = FFmpegOhos.playerOpen(dst);
        if (openR.ok) {
          const playR: NapiResult = FFmpegOhos.playerPlay();
          if (playR.ok) {
            this.status += '\n已开始播放转码结果';
            this.startPolling();
          }
        }
      } finally {
        fs.closeSync(inFile);
        fs.closeSync(outFile);
      }
    } catch (e) {
      const msg = JSON.stringify(e);
      this.status = `转码异常（${format}）: ${msg}`;
      hilog.error(DOMAIN, TAG, 'transcode failed: %{public}s', msg);
    } finally {
      this.busy = false;
    }
  }

  // 走路径的转码：native 直接 open 两个文件，验证 transcodeByPath
  async runTranscodeByPath(format: string): Promise<void> {
    this.stopPolling();
    try {
      FFmpegOhos.playerStop();
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'playerStop before transcode failed: %{public}s', JSON.stringify(e));
    }
    this.busy = true;
    this.status = `路径转码中（${format}）…`;
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('play.m4a');
      const ext = format === 'aac' ? 'm4a' : format;
      const dst = `${ctx.filesDir}/bypath.${ext}`;
      hilog.info(DOMAIN, TAG, 'transcodeByPath %{public}s -> %{public}s (%{public}s)', src, dst, format);
      const res: NapiResult = FFmpegOhos.transcodeByPath(src, dst, format);
      if (!res.ok) {
        this.status = `路径转码失败（${format}）: ${res.data}`;
        return;
      }
      const st = fs.statSync(dst);
      this.status = `路径转码成功（${format}）: ${st.size} B\n${dst}\n${res.data}`;
      const openR: NapiResult = FFmpegOhos.playerOpen(dst);
      if (openR.ok) {
        const playR: NapiResult = FFmpegOhos.playerPlay();
        if (playR.ok) {
          this.status += '\n已开始播放转码结果';
          this.startPolling();
        }
      }
    } catch (e) {
      const msg = JSON.stringify(e);
      this.status = `路径转码异常（${format}）: ${msg}`;
      hilog.error(DOMAIN, TAG, 'transcodeByPath failed: %{public}s', msg);
    } finally {
      this.busy = false;
    }
  }

  // M4.1：把 rawfile 里的 BBB 复制到应用沙箱，再交给 native 解码到 XComponent surface
  async playVideo(bufferMode: boolean = false, cropMode: boolean = false): Promise<void> {
    if (this.surfaceId.length === 0) {
      this.status = '视频 Surface 还没就绪';
      return;
    }
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const name = 'bbb_rabbit_30s.mp4';
      const path = `${ctx.filesDir}/${name}`;
      // 每次都覆盖，避免换测试片后还在放沙箱里的旧文件
      const data = await ctx.resourceManager.getRawFileContent(name);
      const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, data.buffer as ArrayBuffer);
      fs.closeSync(file);
      hilog.info(DOMAIN, TAG, 'video copied to sandbox: %{public}s', path);
      const res: NapiResult = FFmpegOhos.videoPlay(path, this.surfaceId, bufferMode, cropMode);
      this.status = res.ok ? `视频播放已启动(${bufferMode ? 'buffer' : 'surface'},${cropMode ? 'crop' : 'fit'}): ${res.data}` : `视频播放失败: ${res.data}`;
      hilog.info(DOMAIN, TAG, 'videoPlay ok=%{public}s', String(res.ok));
    } catch (e) {
      this.status = `视频播放异常: ${JSON.stringify(e)}`;
      hilog.error(DOMAIN, TAG, 'videoPlay failed: %{public}s', JSON.stringify(e));
    }
  }

  async transcodeVideo(): Promise<void> {
    if (this.surfaceId.length === 0) { this.status = 'Surface 未就绪'; return; }
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const dst = `${ctx.filesDir}/output.mp4`;
      this.status = 'M4.3 转码中…';
      FFmpegOhos.videoTranscode(src, dst, 'mp4');
      for (let i = 0; i < 240; i++) {
        await new Promise<void>((resolve) => setTimeout(resolve, 500));
        const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
        if (s.state === 2) {
          this.status = `转码完成 ${s.size} bytes，播放 output.mp4`;
          FFmpegOhos.videoPlay(dst, this.surfaceId, false, false);
          return;
        }
        if (s.state === 3) {
          this.status = `转码失败 stage=${s.stage}`;
          return;
        }
      }
      this.status = '转码超时';
    } catch (e) {
      this.status = `转码异常: ${JSON.stringify(e)}`;
    }
  }

  async transcodeFormat(format: string): Promise<void> {
    if (format === 'hevc') {
      promptAction.showToast({ message: '当前设备不支持 HEVC 编码', duration: 2500 });
      this.status = '当前设备不支持 HEVC 编码';
      return;
    }
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const dst = `${ctx.filesDir}/output.${format}`;
      FFmpegOhos.videoTranscode(src, dst, format);
      this.status = `M4.4 转码已启动: ${format} -> ${dst}`;
    } catch (e) {
      this.status = `格式转码异常: ${JSON.stringify(e)}`;
    }
  }

  async auditBBatch(): Promise<void> {
    interface AuditCase {
      tag: string;
      ass: boolean;
      style: string;
    }
    const cases: AuditCase[] = [
      { tag: 'ass_none', ass: true, style: '' },
      { tag: 'ass_green', ass: true, style: 'PrimaryColour=&H0000FF00&' },
      { tag: 'srt_none', ass: false, style: '' },
      { tag: 'srt_green', ass: false, style: 'FontName=Source Han Sans CN,FontSize=24,PrimaryColour=&H0000FF00&,Outline=1' }
    ];
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      const fonts = ctx.filesDir;
      for (let c = 0; c < cases.length; c++) {
        const item = cases[c];
        const subName = item.ass ? 'subs_audit2.ass' : 'subs_force.srt';
        await this.ensureSample(subName);
        const dst = `${ctx.filesDir}/audit_b_${item.tag}.mp4`;
        const subPath = `${ctx.filesDir}/${subName}`;
        const opts = '{"trimStart":5,"trimEnd":15,' +
          '"textX":"20","textY":"20",' +
          '"pipPath":"' + src + '","pipEnableStart":2,"pipEnableEnd":6,' +
          '"subtitlePath":"' + subPath + '","fontsDir":"' + fonts + '",' +
          '"subtitleStyle":"' + item.style + '",' +
          '"speed":1.0,"atempo":1.0}';
        this.status = `B 四组 ${item.tag} 中…`;
        FFmpegOhos.videoTranscode(src, dst,
          `watermark|FFmpeg-ohos|center|48|white|${font}|100|1`,
          '720p', 1.0, '', opts);
        for (let i = 0; i < 240; i++) {
          await new Promise<void>((resolve) => setTimeout(resolve, 500));
          const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
          if (s.state === 2) break;
          if (s.state === 3) { this.status = `B ${item.tag} 失败`; return; }
        }
      }
      this.status = 'B 四组完成';
    } catch (e) {
      this.status = `B 四组异常: ${JSON.stringify(e)}`;
    }
  }

  async specialBatch(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      await this.ensureSample('subs_force.srt');
      await this.ensureSample('subs_cn.srt');
      interface SpecialCase { tag: string; sub: string; speed: number; style: string; }
      const cases: SpecialCase[] = [
        { tag: 'colon', sub: 'subs_force.srt', speed: 1.0,
          style: 'FontName=Source Han Sans CN,FontSize=24,PrimaryColour=&H0000FF00&,Outline=1,BorderStyle=1' },
        { tag: 'cn_1x', sub: 'subs_cn.srt', speed: 1.0, style: '' },
        { tag: 'cn_125x', sub: 'subs_cn.srt', speed: 1.25, style: '' }
      ];
      for (let i = 0; i < cases.length; i++) {
        const c = cases[i];
        const dst = `${ctx.filesDir}/special_${c.tag}.mp4`;
        const opts = '{"trimStart":5,"trimEnd":15,"textX":"20","textY":"20",' +
          '"pipPath":"' + src + '","pipEnableStart":2,"pipEnableEnd":6,' +
          '"subtitlePath":"' + ctx.filesDir + '/' + c.sub + '",' +
          '"fontsDir":"' + ctx.filesDir + '","subtitleStyle":"' + c.style + '",' +
          '"speed":' + c.speed.toString() + ',"atempo":' + c.speed.toString() + '}';
        this.status = `特殊测试 ${c.tag}…`;
        FFmpegOhos.videoTranscode(src, dst,
          `watermark|FFmpeg-ohos|center|48|white|${font}|100|1`, '720p', 1.0, '', opts);
        for (let j = 0; j < 240; j++) {
          await new Promise<void>((resolve) => setTimeout(resolve, 500));
          const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
          if (s.state === 2) break;
          if (s.state === 3) { this.status = `特殊 ${c.tag} 失败`; return; }
        }
      }
      this.status = '冒号/中文批量完成';
    } catch (e) { this.status = `特殊异常: ${JSON.stringify(e)}`; }
  }

  async layerTest(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      await this.ensureSample('subs_audit2.ass');
      const dst = `${ctx.filesDir}/audit_layer.mp4`;
      const opts = '{"trimStart":5,"trimEnd":15,' +
        '"textX":"(w-tw)/2","textY":"(h-th)/2",' +
        '"pipPath":"' + src + '","pipEnableStart":2,"pipEnableEnd":6,' +
        '"pipX":"(W-w)/2","pipY":"(H-h)/2",' +
        '"subtitlePath":"' + ctx.filesDir + '/subs_audit2.ass",' +
        '"fontsDir":"' + ctx.filesDir + '","speed":1.0,"atempo":1.0}';
      this.status = '层级重叠测试中…';
      FFmpegOhos.videoTranscode(src, dst,
        `watermark|DRAWTEXT|center|48|white|${font}|100|1`,
        '720p', 1.0, '', opts);
      for (let i = 0; i < 240; i++) {
        await new Promise<void>((resolve) => setTimeout(resolve, 500));
        const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
        if (s.state === 2) { this.status = '层级重叠完成'; return; }
        if (s.state === 3) { this.status = `层级重叠失败 ${s.stage}`; return; }
      }
    } catch (e) { this.status = `层级异常: ${JSON.stringify(e)}`; }
  }

  async degradeBatch(): Promise<void> {
    const modes = ['missing', 'empty_fonts', 'bad_font'];
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      await this.ensureSample('subs_force.srt');
      for (let m = 0; m < modes.length; m++) {
        const mode = modes[m];
        const subPath = mode === 'missing' ? `${ctx.filesDir}/no_such.srt` : `${ctx.filesDir}/subs_force.srt`;
        const fonts = mode === 'empty_fonts' ? '' : ctx.filesDir;
        const style = mode === 'bad_font' ? 'FontName=NoSuchFontXYZ' : '';
        const dst = `${ctx.filesDir}/degrade_${mode}.mp4`;
        const opts = '{"trimStart":5,"trimEnd":15,"textX":"20","textY":"20",' +
          '"subtitlePath":"' + subPath + '","fontsDir":"' + fonts + '",' +
          '"subtitleStyle":"' + style + '","speed":1.0,"atempo":1.0}';
        this.status = `降级测试 ${mode}…`;
        FFmpegOhos.videoTranscode(src, dst,
          `watermark|FFmpeg-ohos|center|48|white|${font}|100|1`, '720p', 1.0, '', opts);
        for (let i = 0; i < 240; i++) {
          await new Promise<void>((resolve) => setTimeout(resolve, 500));
          const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
          if (s.state === 2) break;
          if (s.state === 3) { this.status = `降级 ${mode} 失败`; return; }
        }
      }
      this.status = '降级三组完成';
    } catch (e) { this.status = `降级异常: ${JSON.stringify(e)}`; }
  }

  async subtitleTest(ass: boolean = false, speed: number = 1.25, trimStart: number = 5): Promise<void> {
    try {
      hilog.info(0x0000, 'FFM1', '[ui] subtitleTest ass=%{public}d speed=%{public}s trim=%{public}s',
        ass ? 1 : 0, speed.toFixed(2), trimStart.toFixed(1));
      console.info(`[ui] subtitleTest ass=${ass} speed=${speed} trim=${trimStart}`);
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      const subName = ass ? 'subs_audit2.ass' : 'subs_force.srt';
      await this.ensureSample(subName);
      const dst = `${ctx.filesDir}/${ass ? 'audit_ass' : 'audit_srt'}${speed === 1 ? '_1x' : (speed === 0.5 ? '_slow' : '')}${trimStart !== 5 ? '_trim' : ''}.mp4`;
      const subPath = this.subtitlePath.length > 0 ? this.subtitlePath : `${ctx.filesDir}/${subName}`;
      const fonts = this.fontsDir.length > 0 ? this.fontsDir : ctx.filesDir;
      const style = ass ? '' : 'FontName=Source Han Sans CN,FontSize=24,PrimaryColour=&H00FFFFFF&,Outline=1';
      const opts = '{"trimStart":' + trimStart.toString() + ',"trimEnd":' + (trimStart + 10).toString() + ',' +
        '"brightness":0.08,"contrast":1.10,"saturation":1.15,' +
        '"textX":"20","textY":"20",' +
        '"pipPath":"' + src + '","pipEnableStart":2,"pipEnableEnd":6,' +
        '"subtitlePath":"' + subPath + '","fontsDir":"' + fonts + '",' +
        '"subtitleStyle":"' + style + '",' +
        '"speed":' + speed.toString() + ',"atempo":' + speed.toString() + '}';
      this.status = '字幕审计中…';
      FFmpegOhos.videoTranscode(src, dst,
        `watermark|FFmpeg-ohos|center|48|white|${font}|100|1`,
        '720p', 1.0, '', opts);
      for (let i = 0; i < 240; i++) {
        await new Promise<void>((resolve) => setTimeout(resolve, 500));
        const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
        if (s.state === 2) {
          this.status = `字幕审计完成 ${s.size} bytes`;
          FFmpegOhos.videoPlay(dst, this.surfaceId, false, false);
          return;
        }
        if (s.state === 3) {
          this.status = `字幕审计失败 stage=${s.stage}`;
          return;
        }
      }
    } catch (e) {
      this.status = `字幕测试异常: ${JSON.stringify(e)}`;
    }
  }

  async rotationTest(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      const logo = await this.ensureSample('logo_triangle.png');
      const dst = `${ctx.filesDir}/v03_rotate.mp4`;
      const opts = '{"trimStart":5,"trimEnd":15,"cropW":640,"cropH":480,' +
        '"brightness":0.08,"contrast":1.10,"saturation":1.15,' +
        '"textX":"(w-tw)/2","textY":"(h-th)/2",' +
        '"imageX":"(W-w)/2","imageY":"(H-h)/2",' +
        '"rotate":45,"speed":1.25,"atempo":1.25}';
      this.status = '旋转验证中…';
      FFmpegOhos.videoTranscode(src, dst,
        `watermark|FFmpeg-ohos|center|48|white|${font}|100|1`,
        '720p', 1.0, logo, opts);
      for (let i = 0; i < 240; i++) {
        await new Promise<void>((resolve) => setTimeout(resolve, 500));
        const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
        if (s.state === 2) {
          this.status = `旋转验证完成 ${s.size} bytes`;
          FFmpegOhos.videoPlay(dst, this.surfaceId, false, false);
          return;
        }
        if (s.state === 3) {
          this.status = `旋转验证失败 stage=${s.stage}`;
          return;
        }
      }
    } catch (e) {
      this.status = `旋转验证异常: ${JSON.stringify(e)}`;
    }
  }

  async v03Test(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      const logo = await this.ensureSample('logo_triangle.png');
      const dst = `${ctx.filesDir}/v03_mix.mp4`;
      const opts = '{"trimStart":5,"trimEnd":15,"cropW":640,"cropH":480,' +
        '"brightness":0.08,"contrast":1.10,"saturation":1.15,' +
        '"textX":"(w-tw)/2","textY":"(h-th)/2",' +
        '"imageX":"W-w-20","imageY":"H-h-20",' +
        '"pipPath":"' + src + '","pipEnableStart":2,"pipEnableEnd":6,' +
        '"rotate":45,"speed":1.25,"atempo":1.25,"freqShift":80,"echo":1}';
      this.status = 'V0.3 组合测试中…';
      FFmpegOhos.videoTranscode(src, dst,
        `watermark|FFmpeg-ohos|center|48|white|${font}|100|1`,
        '720p', 1.0, '', opts);
      for (let i = 0; i < 240; i++) {
        await new Promise<void>((resolve) => setTimeout(resolve, 500));
        const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
        if (s.state === 2) {
          this.status = `V0.3 完成 ${s.size} bytes`;
          FFmpegOhos.videoPlay(dst, this.surfaceId, false, false);
          return;
        }
        if (s.state === 3) {
          this.status = `V0.3 失败 stage=${s.stage}`;
          return;
        }
      }
    } catch (e) {
      this.status = `V0.3 异常: ${JSON.stringify(e)}`;
    }
  }

  async watermarkVideo(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const font = await this.ensureSample('SourceHanSansCN-Regular.otf');
      const logo = await this.ensureSample('logo_triangle.png');
      const pr: NapiResult = FFmpegOhos.imageProbe(logo);
      this.status = `LOGO ${pr.data}`;
      const dst = `${ctx.filesDir}/watermark.mp4`;
      this.status = 'M4.5 水印转码中…';
      FFmpegOhos.videoTranscode(src, dst,
        `watermark|${this.wmText}|${this.wmPosition}|${this.wmSize}|${this.wmColor}|${font}|${this.wmAlpha}|${this.wmBold ? 1 : 0}`,
        this.resolution,
        this.convertVolume,
        logo);
      for (let i = 0; i < 240; i++) {
        await new Promise<void>((resolve) => setTimeout(resolve, 500));
        const s = JSON.parse(FFmpegOhos.videoTranscodeStatus().data) as Record<string, number>;
        if (s.state === 2) {
          this.status = `水印完成 ${s.size} bytes`;
          FFmpegOhos.videoPlay(dst, this.surfaceId, false, false);
          return;
        }
        if (s.state === 3) {
          this.status = `水印失败 stage=${s.stage}`;
          return;
        }
      }
    } catch (e) {
      this.status = `水印异常: ${JSON.stringify(e)}`;
    }
  }

  async extractVideo(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const dst = `${ctx.filesDir}/output.h264`;
      const res: NapiResult = FFmpegOhos.extractMedia(src, dst, 'video');
      this.status = res.ok ? `提取视频完成: ${res.data}` : `提取视频失败: ${res.data}`;
    } catch (e) {
      this.status = `提取视频异常: ${JSON.stringify(e)}`;
    }
  }

  async extractAudio(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = await this.ensureSample('bbb_rabbit_30s.mp4');
      const dst = `${ctx.filesDir}/output.aac`;
      this.status = 'M4.4 提取音频中…';
      const res: NapiResult = FFmpegOhos.extractMedia(src, dst, 'audio');
      this.status = res.ok ? `提取完成: ${res.data}` : `提取失败: ${res.data}`;
    } catch (e) {
      this.status = `提取异常: ${JSON.stringify(e)}`;
    }
  }

  // M3.0：探测系统硬解能力（AAC/MP3/FLAC 的 capability、支持采样率、能否 Configure/Start）
  runHardwareProbe(): void {
    try {
      const res: NapiResult = FFmpegOhos.hardwareProbe();
      this.status = res.ok ? `硬解探针完成，详见下方日志` : `探针失败: ${res.data}`;
      this.logText = res.ok ? res.data : res.data;
      hilog.info(DOMAIN, TAG, 'hardwareProbe ok=%{public}s', String(res.ok));
    } catch (e) {
      this.status = `探针异常: ${JSON.stringify(e)}`;
      hilog.error(DOMAIN, TAG, 'hardwareProbe failed: %{public}s', JSON.stringify(e));
    }
  }

  // M4.0：探测系统视频硬解（H.264/H.265 capability、最大分辨率、能否创建解码器）
  runVideoProbe(): void {
    try {
      const res: NapiResult = FFmpegOhos.videoProbe();
      this.status = res.ok ? `视频探针完成：${res.data}` : `视频探针失败: ${res.data}`;
      this.logText = res.ok ? res.data : res.data;
      hilog.info(DOMAIN, TAG, 'videoProbe ok=%{public}s', String(res.ok));
    } catch (e) {
      this.status = `视频探针异常: ${JSON.stringify(e)}`;
      hilog.error(DOMAIN, TAG, 'videoProbe failed: %{public}s', JSON.stringify(e));
    }
  }

  startPolling(): void {
    this.stopPolling();
    this.timer = setInterval(() => {
      this.status = FFmpegOhos.playerStatus().data;
    }, 500);
  }

  stopPolling(): void {
    if (this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  async runTest(): Promise<void> {
    this.busy = true;
    this.logText = '';
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const names: string[] = ['test.m4a', 'test.flac', 'test.opus'];
      let out = '';
      for (const name of names) {
        const raw = await ctx.resourceManager.getRawFileContent(name);
        const src = `${ctx.filesDir}/${name}`;
        const file = fs.openSync(src, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.writeSync(file.fd, raw.buffer as ArrayBuffer);
        fs.closeSync(file);

        const info: string = FFmpegOhos.probe(src);
        const stat: string = FFmpegOhos.readPackets(src);
        const wav = `${ctx.filesDir}/${name}.wav`;
        const res: string = FFmpegOhos.decodeToWav(src, wav);
        out += `${name}\n  open:   ${info}\n  read:   ${stat}\n  decode: ${res}\n\n`;
      }
      this.logText = out;
    } catch (e) {
      this.logText = `异常: ${JSON.stringify(e)}`;
    }
    this.busy = false;
  }
}
