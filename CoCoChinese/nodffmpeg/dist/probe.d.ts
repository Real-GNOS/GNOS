import type { FFprobeMetadata } from './types.js';
export declare function ffprobe(filePath: string): Promise<FFprobeMetadata>;
export declare function getDuration(filePath: string): Promise<number>;
export declare function getFormat(filePath: string): Promise<FFprobeMetadata['format']>;
export declare function getStreams(filePath: string): Promise<FFprobeMetadata['streams']>;
//# sourceMappingURL=probe.d.ts.map