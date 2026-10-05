import { execSync } from 'child_process'
import path from 'path'
import fs from 'fs/promises'

interface SegmentMeta {
  url: string
  start: number
  end: number
}

interface ResolutionManifest {
  width: number
  height: number
  videoInit: string
  audioInit: string
  videoSegments: SegmentMeta[]
  audioSegments: SegmentMeta[]
  videoCodec: string
  audioCodec: string
}

interface VideoJson {
  duration: number
  segmentDuration: number
  resolutions: Record<string, ResolutionManifest>
}

function exec(cmd: string): string {
  return execSync(cmd, { encoding: 'utf-8', maxBuffer: 50 * 1024 * 1024 })
}

function ffprobeJson(filePath: string): any {
  const out = exec(`ffprobe -v quiet -print_format json -show_format -show_streams "${filePath}"`)
  return JSON.parse(out)
}

function parseDuration(filePath: string): number {
  const info = ffprobeJson(filePath)
  return parseFloat(info.format?.duration || '0')
}

function parseCodecs(filePath: string): { videoCodec: string; audioCodec: string; width: number; height: number } {
  const info = ffprobeJson(filePath)
  const v = info.streams?.find((s: any) => s.codec_type === 'video')
  const a = info.streams?.find((s: any) => s.codec_type === 'audio')

  let videoCodec = 'avc1.64001e'
  if (v?.codec_name === 'h264') {
    const profile = (v.profile || 'High').toLowerCase().replace(/[^a-z]/g, '')
    const level = String(v.level || 30).replace('.', '')
    videoCodec = `avc1.${profile.slice(0, 2) === 'hi' ? '64' : profile.slice(0, 2) === 'main' ? '4d' : '64'}00${level.padStart(2, '0')}`
  }

  let audioCodec = 'mp4a.40.2'
  if (a?.codec_name === 'aac') audioCodec = 'mp4a.40.2'

  return {
    videoCodec,
    audioCodec,
    width: v?.width || 1920,
    height: v?.height || 1080,
  }
}

async function listM4SFiles(dir: string): Promise<string[]> {
  try {
    const files = await fs.readdir(dir)
    return files.filter(f => f.endsWith('.m4s')).sort()
  } catch {
    return []
  }
}

async function transcodeResolution(
  inputPath: string,
  resDir: string,
  scale: string,
  width: number,
  height: number,
  segmentDuration: number,
): Promise<ResolutionManifest | null> {
  await fs.mkdir(resDir, { recursive: true })

  const mpdPath = path.join(resDir, 'dash.mpd')

  try {
    exec(
      `ffmpeg -y -i "${inputPath}" ` +
      `-c:v libx264 -preset fast -crf 23 -vf "scale=${scale}" -pix_fmt yuv420p ` +
      `-c:a aac -b:a 128k ` +
      `-f dash ` +
      `-seg_duration ${segmentDuration} ` +
      `-single_file 0 ` +
      `-use_timeline 0 ` +
      `-init_seg_name 'init-$RepresentationID$.m4s' ` +
      `-media_seg_name 'seg-$RepresentationID$-$Number%.m4s' ` +
      `-adaptation_sets "id=0,streams=v id=1,streams=a" ` +
      `-seg_type mp4 ` +
      `"${mpdPath}"`
    )
  } catch (e) {
    console.error(`fMP4 transcoding failed for ${resDir}:`, e)
    return null
  }

  const files = await listM4SFiles(resDir)
  if (files.length === 0) return null

  const initFiles = files.filter(f => f.startsWith('init-'))
  const segFiles = files.filter(f => f.startsWith('seg-'))

  const videoInit = initFiles.find(f => f.includes('-v')) || initFiles[0] || ''
  const audioInit = initFiles.find(f => f.includes('-a')) || initFiles[1] || ''

  const videoSegs = segFiles
    .filter(f => f.includes('-v'))
    .sort((a, b) => {
      const na = parseInt(a.match(/-(\d+)\.m4s$/)?.[1] || '0')
      const nb = parseInt(b.match(/-(\d+)\.m4s$/)?.[1] || '0')
      return na - nb
    })

  const audioSegs = segFiles
    .filter(f => f.includes('-a'))
    .sort((a, b) => {
      const na = parseInt(a.match(/-(\d+)\.m4s$/)?.[1] || '0')
      const nb = parseInt(b.match(/-(\d+)\.m4s$/)?.[1] || '0')
      return na - nb
    })

  const codecs = parseCodecs(inputPath)

  const videoSegments: SegmentMeta[] = videoSegs.map((f, i) => ({
    url: `${f}`,
    start: i * segmentDuration,
    end: (i + 1) * segmentDuration,
  }))

  const audioSegments: SegmentMeta[] = audioSegs.map((f, i) => ({
    url: `${f}`,
    start: i * segmentDuration,
    end: (i + 1) * segmentDuration,
  }))

  await fs.unlink(mpdPath).catch(() => {})

  return {
    width,
    height,
    videoInit: `${videoInit}`,
    audioInit: `${audioInit}`,
    videoSegments,
    audioSegments,
    videoCodec: codecs.videoCodec,
    audioCodec: codecs.audioCodec,
  }
}

export async function transcodeAllFMP4(
  inputPath: string,
  baseDir: string,
  slug: string,
): Promise<{ videoJsonPath: string; success: boolean }> {
  const outDir = path.join(baseDir, slug)
  await fs.mkdir(outDir, { recursive: true })

  const duration = parseDuration(inputPath)
  const segmentDuration = 4

  const profiles: Record<string, { scale: string; width: number; height: number }> = {
    '360p': { scale: '-2:360', width: 640, height: 360 },
    '720p': { scale: '-2:720', width: 1280, height: 720 },
    '1080p': { scale: '-2:1080', width: 1920, height: 1080 },
  }

  const resolutions: Record<string, ResolutionManifest> = {}
  let anySuccess = false

  for (const [name, profile] of Object.entries(profiles)) {
    const resDir = path.join(outDir, name)
    const result = await transcodeResolution(
      inputPath, resDir, profile.scale,
      profile.width, profile.height, segmentDuration,
    )
    if (result) {
      anySuccess = true
      result.videoInit = `${name}/${result.videoInit}`
      result.audioInit = `${name}/${result.audioInit}`
      result.videoSegments = result.videoSegments.map(s => ({
        ...s, url: `${name}/${s.url}`,
      }))
      result.audioSegments = result.audioSegments.map(s => ({
        ...s, url: `${name}/${s.url}`,
      }))
      resolutions[name] = result
    }
  }

  const videoJson: VideoJson = { duration, segmentDuration, resolutions }
  const videoJsonPath = path.join(outDir, 'video.json')
  await fs.writeFile(videoJsonPath, JSON.stringify(videoJson))

  return { videoJsonPath, success: anySuccess }
}
