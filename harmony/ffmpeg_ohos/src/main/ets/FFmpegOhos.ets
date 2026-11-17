import native from 'libentry.so';

export interface NapiResult {
  ok: boolean;
  data: string;
}

export class FFmpegOhos {
  static videoTranscode(srcPath: string, dstPath: string, format?: string,
    resolution?: string, volume?: number, imagePath?: string, optionsJson?: string): NapiResult {
    return native.videoTranscode(srcPath, dstPath, format, resolution, volume, imagePath, optionsJson) as NapiResult;
  }
  static videoPlay(path: string, surfaceId: string, bufferMode?: boolean, cropMode?: boolean): NapiResult {
    return native.videoPlay(path, surfaceId, bufferMode, cropMode) as NapiResult;
  }
  static videoStop(): NapiResult { return native.videoStop() as NapiResult; }
  static videoProbe(): NapiResult { return native.videoProbe() as NapiResult; }
  static videoTranscodeStatus(): NapiResult { return native.videoTranscodeStatus() as NapiResult; }
  static extractMedia(srcPath: string, dstPath: string, type: string): NapiResult {
    return native.extractMedia(srcPath, dstPath, type) as NapiResult;
  }
  static setVolume(volume: number): NapiResult { return native.setVolume(volume) as NapiResult; }
  static decoderSetHardware(on: boolean): NapiResult { return native.decoderSetHardware(on) as NapiResult; }
  static imageProbe(path: string): NapiResult { return native.imageProbe(path) as NapiResult; }

  static probe(path: string): string { return native.probe(path); }
  static readPackets(path: string): string { return native.readPackets(path); }
  static decodeToWav(path: string, wavPath: string): string { return native.decodeToWav(path, wavPath); }
  static playerOpen(path: string): NapiResult { return native.playerOpen(path) as NapiResult; }
  static playerOpenFd(fd: number): NapiResult { return native.playerOpenFd(fd) as NapiResult; }
  static playerPlay(): NapiResult { return native.playerPlay() as NapiResult; }
  static playerPause(): NapiResult { return native.playerPause() as NapiResult; }
  static playerStop(): NapiResult { return native.playerStop() as NapiResult; }
  static playerStatus(): NapiResult { return native.playerStatus() as NapiResult; }
  static playerRelease(): NapiResult { return native.playerRelease() as NapiResult; }
  static transcodeToAac(srcFd: number, dstFd: number): NapiResult {
    return native.transcodeToAac(srcFd, dstFd) as NapiResult;
  }
  static transcodeTo(srcFd: number, dstFd: number, format: string, bitrateKbps?: number): NapiResult {
    return native.transcodeTo(srcFd, dstFd, format, bitrateKbps) as NapiResult;
  }
  static transcodeByPath(srcPath: string, dstPath: string, format: string, bitrateKbps?: number): NapiResult {
    return native.transcodeByPath(srcPath, dstPath, format, bitrateKbps) as NapiResult;
  }
  static hardwareProbe(): NapiResult { return native.hardwareProbe() as NapiResult; }
}
