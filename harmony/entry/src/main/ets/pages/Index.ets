import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
// NAPI 模块：libentry.so，C 侧 nm_modname = "entry"
import native from 'libentry.so';

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
  private timer: number = -1;

  aboutToDisappear(): void {
    this.stopPolling();
  }

  build() {
    Column({ space: 12 }) {
      Text('ffmpeg-for-HarmonyOS · M2 播放自检')
        .fontSize(18)
        .fontWeight(FontWeight.Bold)

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

      Button(this.hardwareOn ? '硬解开关：开（AAC）' : '硬解开关：关（走软解）')
        .fontSize(13)
        .onClick(() => {
          this.hardwareOn = !this.hardwareOn;
          try {
            native.decoderSetHardware(this.hardwareOn);
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
    .height('100%')
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
      const openR = native.playerOpen(src);
      if (!openR.ok) {
        this.status = `打开失败: ${openR.data}`;
        return;
      }
      const playR = native.playerPlay();
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
      this.status = native.playerStop().data;
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
      native.playerStop();
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
      const openR = native.playerOpen(local);
      if (!openR.ok) {
        this.status = `解码器打开失败：${openR.data}`;
        hilog.error(DOMAIN, TAG, 'playerOpen !ok: %{public}s', openR.data);
        return;
      }
      const playR = native.playerPlay();
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
      native.playerStop();
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
        const res = native.transcodeTo(inFile.fd, outFile.fd, format);
        hilog.info(DOMAIN, TAG, 'transcode result ok=%{public}s data=%{public}s', String(res.ok), res.data);
        if (!res.ok) {
          this.status = `转码失败（${format}）: ${res.data}`;
          return;
        }
        const st = fs.statSync(dst);
        this.status = `转码成功（${format}）: ${st.size} B\n${dst}\n${res.data}`;

        // 立刻播放转码结果，证明产物可用
        const openR = native.playerOpen(dst);
        if (openR.ok) {
          const playR = native.playerPlay();
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
      native.playerStop();
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
      const res = native.transcodeByPath(src, dst, format);
      if (!res.ok) {
        this.status = `路径转码失败（${format}）: ${res.data}`;
        return;
      }
      const st = fs.statSync(dst);
      this.status = `路径转码成功（${format}）: ${st.size} B\n${dst}\n${res.data}`;
      const openR = native.playerOpen(dst);
      if (openR.ok) {
        const playR = native.playerPlay();
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

  // M3.0：探测系统硬解能力（AAC/MP3/FLAC 的 capability、支持采样率、能否 Configure/Start）
  runHardwareProbe(): void {
    try {
      const res = native.hardwareProbe();
      this.status = res.ok ? `硬解探针完成，详见下方日志` : `探针失败: ${res.data}`;
      this.logText = res.ok ? res.data : res.data;
      hilog.info(DOMAIN, TAG, 'hardwareProbe ok=%{public}s', String(res.ok));
    } catch (e) {
      this.status = `探针异常: ${JSON.stringify(e)}`;
      hilog.error(DOMAIN, TAG, 'hardwareProbe failed: %{public}s', JSON.stringify(e));
    }
  }

  startPolling(): void {
    this.stopPolling();
    this.timer = setInterval(() => {
      this.status = native.playerStatus().data;
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

        const info: string = native.probe(src);
        const stat: string = native.readPackets(src);
        const wav = `${ctx.filesDir}/${name}.wav`;
        const res: string = native.decodeToWav(src, wav);
        out += `${name}\n  open:   ${info}\n  read:   ${stat}\n  decode: ${res}\n\n`;
      }
      this.logText = out;
    } catch (e) {
      this.logText = `异常: ${JSON.stringify(e)}`;
    }
    this.busy = false;
  }
}
