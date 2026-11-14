export interface NapiResult {
  ok: boolean;
  data: string;
}

export declare class FFmpegOhos {
  static videoTranscode(srcPath: string, dstPath: string, format?: string,
    resolution?: string, volume?: number, imagePath?: string): NapiResult;
  static videoPlay(path: string, surfaceId: string, bufferMode?: boolean, cropMode?: boolean): NapiResult;
  static videoStop(): NapiResult;
  static videoProbe(): NapiResult;
  static videoTranscodeStatus(): NapiResult;
  static extractMedia(srcPath: string, dstPath: string, type: string): NapiResult;
  static setVolume(volume: number): NapiResult;
  static decoderSetHardware(on: boolean): NapiResult;
  static imageProbe(path: string): NapiResult;
  static probe(path: string): string;
  static readPackets(path: string): string;
  static decodeToWav(path: string, wavPath: string): string;
  static playerOpen(path: string): NapiResult;
  static playerOpenFd(fd: number): NapiResult;
  static playerPlay(): NapiResult;
  static playerPause(): NapiResult;
  static playerStop(): NapiResult;
  static playerStatus(): NapiResult;
  static playerRelease(): NapiResult;
  static transcodeToAac(srcFd: number, dstFd: number): NapiResult;
  static transcodeTo(srcFd: number, dstFd: number, format: string, bitrateKbps?: number): NapiResult;
  static transcodeByPath(srcPath: string, dstPath: string, format: string, bitrateKbps?: number): NapiResult;
  static hardwareProbe(): NapiResult;
}
