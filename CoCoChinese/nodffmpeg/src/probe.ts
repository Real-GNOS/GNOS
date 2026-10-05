import { execFile } from 'child_process'
import { promisify } from 'util'
import { getFFprobePath } from './binary.js'
import type { FFprobeMetadata } from './types.js'

const execFileAsync = promisify(execFile)

export async function ffprobe(filePath: string): Promise<FFprobeMetadata> {
  const bin = getFFprobePath()
  try {
    const { stdout } = await execFileAsync(bin, [
      '-v', 'quiet',
      '-print_format', 'json',
      '-show_format',
      '-show_streams',
      filePath,
    ])
    return JSON.parse(stdout) as FFprobeMetadata
  } catch (err) {
    throw new Error(`ffprobe failed for "${filePath}": ${err instanceof Error ? err.message : String(err)}`)
  }
}

export async function getDuration(filePath: string): Promise<number> {
  const meta = await ffprobe(filePath)
  return meta?.format?.duration ?? 0
}

export async function getFormat(filePath: string): Promise<FFprobeMetadata['format']> {
  const meta = await ffprobe(filePath)
  return meta?.format
}

export async function getStreams(filePath: string): Promise<FFprobeMetadata['streams']> {
  const meta = await ffprobe(filePath)
  return meta?.streams
}
