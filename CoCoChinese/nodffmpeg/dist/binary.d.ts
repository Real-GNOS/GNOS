export declare function getFFmpegPath(): string;
export declare function getFFprobePath(): string;
export declare function getFFmpegPathAsync(): Promise<string>;
export declare function getFFprobePathAsync(): Promise<string>;
export declare function setFFmpegPath(p: string): void;
export declare function setFFprobePath(p: string): void;
export declare function checkBinaryAvailable(): {
    ffmpeg: boolean;
    ffprobe: boolean;
    errors: string[];
};
//# sourceMappingURL=binary.d.ts.map