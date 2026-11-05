// player* 返回结构化结果：用 ok 判断成败，不要把 data 当 JSON 嗅探
export interface NapiResult {
  ok: boolean;
  data: string;
}

declare const native: {
  // M1：整文件自检（返回 JSON 字符串）
  probe: (path: string) => string;
  readPackets: (path: string) => string;
  decodeToWav: (path: string, wavPath: string) => string;
  // M2：播放器（单实例）
  playerOpen: (path: string) => NapiResult;
  playerOpenFd: (fd: number) => NapiResult;
  playerPlay: () => NapiResult;
  playerPause: () => NapiResult;
  playerStop: () => NapiResult;
  playerStatus: () => NapiResult;
  playerRelease: () => NapiResult;
  // 转码示例：srcFd 解出的第一条音轨 → AAC(ipod/m4a) 写入 dstFd
  transcodeToAac: (srcFd: number, dstFd: number) => NapiResult;
  // 通用转码：format 支持 "aac" / "mp3" / "opus" / "flac" / "wav"
  // 批量转码默认软解：底层不会创建 OH_AudioCodec 实例（硬解实例有上限，转码瓶颈在编码器）
  transcodeTo: (srcFd: number, dstFd: number, format: string) => NapiResult;
  // 走路径的转码：native 侧直接 open 两个文件，省掉 ArkTS 取 fd
  transcodeByPath: (srcPath: string, dstPath: string, format: string) => NapiResult;
  // M3.0 探针：查系统硬解对 AAC/MP3/FLAC 的能力
  hardwareProbe: () => NapiResult;
  // M3.1 硬解开关（A/B 对比用），影响下一次 Open
  decoderSetHardware: (on: boolean) => NapiResult;
};

export default native;
