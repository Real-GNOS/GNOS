import { ffmpeg, ffprobe } from 'nodffmpeg'
import path from 'path'
import fs from 'fs/promises'

export async function getVideoDuration(filePath: string): Promise<string> {
  const metadata = await ffprobe(filePath)
  if (!metadata?.format?.duration) return '00:00'
  const totalSec = Math.round(metadata.format.duration)
  const h = Math.floor(totalSec / 3600)
  const m = Math.floor((totalSec % 3600) / 60)
  const s = totalSec % 60
  if (h > 0) {
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
}

export function standardizeVideo(inputPath: string, outputPath: string): Promise<void> {
  return new Promise((resolve, reject) => {
    ffmpeg(inputPath)
      .videoCodec('libx264')
      .audioCodec('aac')
      .preset('fast')
      .crf(23)
      .addOptions(['-movflags', '+faststart'])
      .pixFmt('yuv420p')
      .on('end', () => resolve())
      .on('error', (err) => reject(err))
      .save(outputPath)
  })
}

export async function processVideo(inputPath: string, outputPath: string): Promise<string> {
  try {
    await standardizeVideo(inputPath, outputPath)
    if (inputPath !== outputPath) {
      await fs.unlink(inputPath).catch(() => {})
    }
  } catch {
    if (inputPath !== outputPath) {
      await fs.copyFile(inputPath, outputPath).catch(() => {})
      await fs.unlink(inputPath).catch(() => {})
    }
  }
  return getVideoDuration(outputPath)
}

async function transcodeResolution(inputPath: string, resDir: string, scale: string, bitrate: string): Promise<void> {
  await fs.mkdir(resDir, { recursive: true })
  return new Promise((resolve, reject) => {
    ffmpeg(inputPath)
      .videoCodec('libx264')
      .videoFilter(`scale=${scale}`)
      .videoBitrate(bitrate.replace('k', ''))
      .preset('fast')
      .crf(23)
      .pixFmt('yuv420p')
      .audioCodec('aac')
      .audioBitrate(128)
      .outputOptions([
        '-f', 'hls',
        '-hls_time', '10',
        '-hls_playlist_type', 'vod',
        '-hls_list_size', '0',
        '-force_key_frames', 'expr:gte(t,n_forced*10)',
        '-hls_segment_filename', path.join(resDir, 'seg%d.ts'),
        '-hls_flags', 'independent_segments',
      ])
      .output(path.join(resDir, 'index.m3u8'))
      .on('end', () => resolve())
      .on('error', (err) => reject(err))
      .run()
  })
}

function writeMasterPlaylist(outputDir: string, resolutions: { name: string; bandwidth: number; resolution: string }[]): Promise<void> {
  const lines = ['#EXTM3U', '#EXT-X-VERSION:3', '']
  for (const r of resolutions) {
    lines.push(`#EXT-X-STREAM-INF:BANDWIDTH=${r.bandwidth},RESOLUTION=${r.resolution}`)
    lines.push(`${r.name}/index.m3u8`)
    lines.push('')
  }
  return fs.writeFile(path.join(outputDir, 'master.m3u8'), lines.join('\n'))
}

export async function patchEndlist(baseDir: string): Promise<number> {
  let patched = 0
  for (const d of ['360p', '720p', '1080p']) {
    const m3u8 = path.join(baseDir, d, 'index.m3u8')
    try {
      let content = await fs.readFile(m3u8, 'utf-8')
      if (content.includes('#EXT-X-ENDLIST')) continue
      if (!content.endsWith('\n')) content += '\n'
      content += '#EXT-X-ENDLIST\n'
      await fs.writeFile(m3u8, content)
      patched++
    } catch {}
  }
  return patched
}

export async function transcodeAllResolutions(inputPath: string, outputDir: string, slug: string): Promise<{
  videoUrl360p: string
  videoUrl720p: string
  videoUrl1080p: string
  videoUrlHls: string
}> {
  const baseDir = path.join(outputDir, slug)
  await fs.mkdir(baseDir, { recursive: true })

  const videoUrl360p = `/videos/${slug}/360p/index.m3u8`
  const videoUrl720p = `/videos/${slug}/720p/index.m3u8`
  const videoUrl1080p = `/videos/${slug}/1080p/index.m3u8`
  const videoUrlHls = `/videos/${slug}/master.m3u8`

  const profiles: Record<string, { scale: string; bitrate: string; bandwidth: number; resolution: string }> = {
    '360p': { scale: '-2:360', bitrate: '800k', bandwidth: 1000000, resolution: '640x360' },
    '720p': { scale: '-2:720', bitrate: '2500k', bandwidth: 2800000, resolution: '1280x720' },
    '1080p': { scale: '-2:1080', bitrate: '5000k', bandwidth: 5500000, resolution: '1920x1080' },
  }

  await Promise.all([
    transcodeResolution(inputPath, path.join(baseDir, '360p'), profiles['360p'].scale, profiles['360p'].bitrate),
    transcodeResolution(inputPath, path.join(baseDir, '720p'), profiles['720p'].scale, profiles['720p'].bitrate),
    transcodeResolution(inputPath, path.join(baseDir, '1080p'), profiles['1080p'].scale, profiles['1080p'].bitrate),
  ])

  await writeMasterPlaylist(baseDir, [
    { name: '360p', bandwidth: profiles['360p'].bandwidth, resolution: profiles['360p'].resolution },
    { name: '720p', bandwidth: profiles['720p'].bandwidth, resolution: profiles['720p'].resolution },
    { name: '1080p', bandwidth: profiles['1080p'].bandwidth, resolution: profiles['1080p'].resolution },
  ])

  return { videoUrl360p, videoUrl720p, videoUrl1080p, videoUrlHls }
}
