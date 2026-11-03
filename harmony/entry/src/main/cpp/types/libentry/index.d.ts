declare const native: {
  probe: (path: string) => string;
  readPackets: (path: string) => string;
  decodeToWav: (path: string, wavPath: string) => string;
};

export default native;
