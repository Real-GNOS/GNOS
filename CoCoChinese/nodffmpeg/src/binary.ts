import which from 'which'
import { execFileSync, execSync } from 'child_process'
import fs from 'fs'
import path from 'path'

let ffmpegPath = 'ffmpeg'
let ffprobePath = 'ffprobe'
let resolving: Promise<void> | null = null
let resolved = false

async function resolveBinaryAsync(name: 'ffmpeg' | 'ffprobe'): Promise<string> {
  const envKey = name === 'ffmpeg' ? 'FFMPEG_PATH' : 'FFPROBE_PATH'
  const envVal = process.env[envKey]
  if (envVal && fs.existsSync(envVal)) return envVal

  const found = await which(name, { nothrow: true })
  if (found) return found

  if (process.platform === 'win32') {
    const candidates = [
      path.join('C:', 'Program Files', 'FFmpeg', 'bin', `${name}.exe`),
      path.join('C:', 'ffmpeg', 'bin', `${name}.exe`),
      path.join('C:', 'Program Files (x86)', 'FFmpeg', 'bin', `${name}.exe`),
    ]
    for (const c of candidates) {
      if (fs.existsSync(c)) return c
    }
    try {
      const out = execSync(`where ${name}`, { encoding: 'utf-8', stdio: 'pipe', timeout: 3000 })
      const lines = out.split(/\r?\n/).filter(Boolean)
      if (lines.length) return lines[0].trim()
    } catch { }
  } else {
    try {
      const out = execSync(`which ${name}`, { encoding: 'utf-8', stdio: 'pipe', timeout: 3000 })
      if (out) return out.trim()
    } catch { }
  }

  const msg = `Cannot locate ${name} executable. Please install FFmpeg or set ${envKey} environment variable.`
  throw new Error(msg)
}

function resolveBinarySync(name: 'ffmpeg' | 'ffprobe'): string {
  const envKey = name === 'ffmpeg' ? 'FFMPEG_PATH' : 'FFPROBE_PATH'
  const envVal = process.env[envKey]
  if (envVal && fs.existsSync(envVal)) return envVal

  try {
    const found = which.sync(name, { nothrow: true })
    if (found) return found
  } catch { }

  if (process.platform === 'win32') {
    const candidates = [
      path.join('C:', 'Program Files', 'FFmpeg', 'bin', `${name}.exe`),
      path.join('C:', 'ffmpeg', 'bin', `${name}.exe`),
      path.join('C:', 'Program Files (x86)', 'FFmpeg', 'bin', `${name}.exe`),
    ]
    for (const c of candidates) {
      if (fs.existsSync(c)) return c
    }
    try {
      const out = execSync(`where ${name}`, { encoding: 'utf-8', stdio: 'pipe', timeout: 3000 })
      const lines = out.split(/\r?\n/).filter(Boolean)
      if (lines.length) return lines[0].trim()
    } catch { }
  } else {
    try {
      const out = execSync(`which ${name}`, { encoding: 'utf-8', stdio: 'pipe', timeout: 3000 })
      if (out) return out.trim()
    } catch { }
  }

  throw new Error(`Cannot locate ${name}. Please install FFmpeg or set ${envKey}.`)
}

async function resolvePathsAsync(): Promise<void> {
  if (resolved) return
  if (resolving) return resolving

  resolving = (async () => {
    try {
      const [ff, fp] = await Promise.all([
        resolveBinaryAsync('ffmpeg'),
        resolveBinaryAsync('ffprobe'),
      ])
      ffmpegPath = ff
      ffprobePath = fp
      resolved = true
    } finally {
      resolving = null
    }
  })()
  return resolving
}

function resolvePathsSync(): void {
  if (resolved) return
  ffmpegPath = resolveBinarySync('ffmpeg')
  ffprobePath = resolveBinarySync('ffprobe')
  resolved = true
}

export function getFFmpegPath(): string {
  resolvePathsSync()
  return ffmpegPath
}

export function getFFprobePath(): string {
  resolvePathsSync()
  return ffprobePath
}

export async function getFFmpegPathAsync(): Promise<string> {
  await resolvePathsAsync()
  return ffmpegPath
}

export async function getFFprobePathAsync(): Promise<string> {
  await resolvePathsAsync()
  return ffprobePath
}

export function setFFmpegPath(p: string): void {
  ffmpegPath = p
  resolved = true
}

export function setFFprobePath(p: string): void {
  ffprobePath = p
  resolved = true
}

export function checkBinaryAvailable(): { ffmpeg: boolean; ffprobe: boolean; errors: string[] } {
  const errors: string[] = []
  const ff = getFFmpegPath()
  const fp = getFFprobePath()

  let ffmpegOk = false
  try {
    execFileSync(ff, ['-version'], { stdio: 'pipe', timeout: 5000 })
    ffmpegOk = true
  } catch (e: any) {
    errors.push(`ffmpeg: ${e.message || String(e)}`)
  }

  let ffprobeOk = false
  try {
    execFileSync(fp, ['-version'], { stdio: 'pipe', timeout: 5000 })
    ffprobeOk = true
  } catch (e: any) {
    errors.push(`ffprobe: ${e.message || String(e)}`)
  }

  return { ffmpeg: ffmpegOk, ffprobe: ffprobeOk, errors }
}
