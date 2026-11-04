declare const native: {
  // M1：整文件自检
  probe: (path: string) => string;
  readPackets: (path: string) => string;
  decodeToWav: (path: string, wavPath: string) => string;
  // M2：播放器（单实例）
  playerOpen: (path: string) => string;
  playerOpenFd: (fd: number) => string;
  playerPlay: () => string;
  playerPause: () => string;
  playerStop: () => string;
  playerStatus: () => string;
  playerRelease: () => string;
};

export default native;
