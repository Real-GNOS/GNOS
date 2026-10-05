export { FFmpegCommand } from './command.js';
export { ffprobe, getDuration, getFormat, getStreams, } from './probe.js';
export { getFFmpegPath, getFFprobePath, setFFmpegPath, setFFprobePath, checkBinaryAvailable, } from './binary.js';
export type { FFprobeMetadata, FFprobeStream, FFprobeFormat, FFmpegProgress, FFmpegEventHandler, FFmpegProgressHandler, } from './types.js';
import { FFmpegCommand } from './command.js';
export declare function ffmpeg(inputPath: string): FFmpegCommand;
//# sourceMappingURL=index.d.ts.map