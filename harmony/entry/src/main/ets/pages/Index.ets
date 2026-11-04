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

      Text(this.status)
        .fontSize(12)
        .fontFamily('monospace')
        .width('100%')

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
      const openRes: string = native.playerOpen(src);
      const playRes: string = native.playerPlay();
      this.status = `内置样例 play.m4a\nopen: ${openRes}\nplay: ${playRes}`;
      this.startPolling();
    } catch (e) {
      this.status = `异常: ${JSON.stringify(e)}`;
      hilog.error(DOMAIN, TAG, 'onPlay failed: %{public}s', JSON.stringify(e));
    }
  }

  onStop(): void {
    try {
      this.stopPolling();
      this.status = native.playerStop();
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

    this.status = `已选择：${uri}\n正在打开…`;
    hilog.info(DOMAIN, TAG, 'picked uri=%{public}s', uri);

    let fd = -1;
    let openRes = '';
    try {
      const file = fs.openSync(uri, fs.OpenMode.READ_ONLY);
      fd = file.fd;
      hilog.info(DOMAIN, TAG, 'fs.openSync ok, fd=%{public}d', fd);
      try {
        openRes = native.playerOpenFd(fd);
      } finally {
        fs.closeSync(file);
      }
    } catch (e) {
      const msg = JSON.stringify(e);
      this.status = `打开文件失败（fd=${fd}）：${msg}`;
      hilog.error(DOMAIN, TAG, 'open fd failed: %{public}s', msg);
      return;
    }

    hilog.info(DOMAIN, TAG, 'playerOpenFd -> %{public}s', openRes);
    if (openRes.includes('"error"')) {
      this.status = `解码器打开失败：${openRes}`;
      return;
    }

    try {
      const playRes: string = native.playerPlay();
      hilog.info(DOMAIN, TAG, 'playerPlay -> %{public}s', playRes);
      if (playRes.includes('"error"')) {
        this.status = `播放启动失败：${playRes}`;
        return;
      }
      this.status = `file: ${uri}\nplay: ${playRes}`;
      this.startPolling();
    } catch (e) {
      const msg = JSON.stringify(e);
      this.status = `播放异常：${msg}`;
      hilog.error(DOMAIN, TAG, 'playerPlay failed: %{public}s', msg);
    }
  }

  startPolling(): void {
    this.stopPolling();
    this.timer = setInterval(() => {
      this.status = native.playerStatus();
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
