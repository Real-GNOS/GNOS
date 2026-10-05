export { FFmpegCommand } from './command.js';
export { ffprobe, getDuration, getFormat, getStreams, } from './probe.js';
export { getFFmpegPath, getFFprobePath, setFFmpegPath, setFFprobePath, checkBinaryAvailable, } from './binary.js';
import { FFmpegCommand } from './command.js';
export function ffmpeg(inputPath) {
    return new FFmpegCommand(inputPath);
}
//# sourceMappingURL=index.js.map