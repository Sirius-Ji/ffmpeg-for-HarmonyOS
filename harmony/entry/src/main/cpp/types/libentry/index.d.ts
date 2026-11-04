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
};

export default native;
