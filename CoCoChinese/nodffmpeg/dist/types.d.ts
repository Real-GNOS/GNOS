export interface FFprobeStream {
    index?: number;
    codec_name?: string;
    codec_long_name?: string;
    codec_type?: 'video' | 'audio' | 'subtitle' | 'data';
    codec_tag?: string;
    width?: number;
    height?: number;
    coded_width?: number;
    coded_height?: number;
    sample_fmt?: string;
    sample_rate?: string;
    channels?: number;
    channel_layout?: string;
    bits_per_sample?: number;
    r_frame_rate?: string;
    avg_frame_rate?: string;
    time_base?: string;
    duration_ts?: number;
    duration?: string;
    bit_rate?: string;
    nb_frames?: string;
    display_aspect_ratio?: string;
    pix_fmt?: string;
    profile?: string;
    level?: number;
    color_range?: string;
    color_space?: string;
    color_transfer?: string;
    color_primaries?: string;
    field_order?: string;
    refs?: number;
    is_avc?: string;
    nal_length_size?: string;
}
export interface FFprobeFormat {
    filename?: string;
    nb_streams?: number;
    nb_programs?: number;
    format_name?: string;
    format_long_name?: string;
    start_time?: string;
    duration?: number;
    size?: string;
    bit_rate?: string;
    probe_score?: number;
    tags?: Record<string, string>;
}
export interface FFprobeMetadata {
    streams?: FFprobeStream[];
    format?: FFprobeFormat;
    programs?: any[];
    chapters?: any[];
}
export interface FFmpegProgress {
    frame?: number;
    fps?: number;
    bitrate?: number;
    totalSize?: number;
    outTime?: string;
    outTimeMs?: number;
    dup?: number;
    drop?: number;
    speed?: string;
    progress?: 'continue' | 'end';
}
export type FFmpegEventHandler = (data?: any) => void;
export type FFmpegProgressHandler = (progress: FFmpegProgress) => void;
//# sourceMappingURL=types.d.ts.map