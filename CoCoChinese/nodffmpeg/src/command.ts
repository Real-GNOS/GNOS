import { spawn } from 'child_process'
import { Writable } from 'stream'
import { pipeline } from 'stream'
import { getFFmpegPath } from './binary.js'
import type { FFmpegEventHandler, FFmpegProgress, FFmpegProgressHandler } from './types.js'

export class FFmpegCommand {
  private inputPath = ''
  private inputArgs: string[] = []
  private outputArgs: string[] = []
  private outputPath = ''
  private listeners: Record<string, FFmpegEventHandler[]> = {}
  private proc: ReturnType<typeof spawn> | null = null
  private stderrBuffer = ''
  private progressInterval: NodeJS.Timeout | null = null

  constructor(inputPath: string) {
    this.inputPath = inputPath
  }

  on(event: 'end' | 'error' | 'progress', handler: FFmpegEventHandler): this {
    if (!this.listeners[event]) this.listeners[event] = []
    this.listeners[event].push(handler)
    return this
  }

  private emit(event: string, ...args: any[]) {
    for (const h of this.listeners[event] || []) {
      try { h(...args) } catch { }
    }
  }

  input(file: string): this {
    this.inputPath = file
    return this
  }

  inputOptions(options: string[] | string): this {
    const arr = Array.isArray(options) ? options : [options]
    this.inputArgs.push(...arr)
    return this
  }

  videoCodec(codec: string): this { this.outputArgs.push('-c:v', codec); return this }
  audioCodec(codec: string): this { this.outputArgs.push('-c:a', codec); return this }
  videoBitrate(bitrate: string | number): this {
    this.outputArgs.push('-b:v', typeof bitrate === 'number' ? `${bitrate}k` : bitrate)
    return this
  }
  audioBitrate(bitrate: string | number): this {
    this.outputArgs.push('-b:a', typeof bitrate === 'number' ? `${bitrate}k` : bitrate)
    return this
  }
  fps(fps: number): this { this.outputArgs.push('-r', String(fps)); return this }
  size(w: number, h: number): this { this.outputArgs.push('-s', `${w}x${h}`); return this }
  aspect(ratio: string): this { this.outputArgs.push('-aspect', ratio); return this }
  frames(count: number): this { this.outputArgs.push('-frames:v', String(count)); return this }
  duration(duration: string | number): this {
    this.outputArgs.push('-t', typeof duration === 'number' ? String(duration) : duration)
    return this
  }
  seek(time: string | number): this {
    this.outputArgs.push('-ss', typeof time === 'number' ? String(time) : time)
    return this
  }
  preset(preset: string): this { this.outputArgs.push('-preset', preset); return this }
  crf(value: number): this { this.outputArgs.push('-crf', String(value)); return this }
  pixFmt(fmt: string): this { this.outputArgs.push('-pix_fmt', fmt); return this }
  noVideo(): this { this.outputArgs.push('-vn'); return this }
  noAudio(): this { this.outputArgs.push('-an'); return this }
  format(fmt: string): this { this.outputArgs.push('-f', fmt); return this }
  videoFilter(filter: string): this { this.outputArgs.push('-vf', filter); return this }
  audioFilter(filter: string): this { this.outputArgs.push('-af', filter); return this }
  addOptions(options: string[] | string): this {
    const arr = Array.isArray(options) ? options : [options]
    this.outputArgs.push(...arr)
    return this
  }
  outputOptions(options: string[] | string): this {
    const arr = Array.isArray(options) ? options : [options]
    this.outputArgs.push(...arr)
    return this
  }
  output(path: string): this {
    this.outputPath = path
    return this
  }

  save(path: string): this {
    this.outputPath = path
    this.run()
    return this
  }

  pipe(): this {
    this.outputArgs.push('-f', 'matroska')
    this.outputPath = 'pipe:1'
    return this
  }

  pipeTo(dest: Writable): Promise<void> {
    return new Promise((resolve, reject) => {
      this.pipe()
      const proc = this._spawnProcess()
      if (!proc.stdout) return reject(new Error('No stdout stream'))

      pipeline(proc.stdout, dest, (err) => {
        if (err) reject(err)
        else resolve()
      })
    })
  }

  exec(): Promise<{ code: number; stdout: string; stderr: string }> {
    return new Promise((resolve, reject) => {
      const bin = getFFmpegPath()
      const args = this._buildArgs()
      const proc = spawn(bin, args, { windowsHide: true })
      let stdout = ''
      let stderr = ''
      proc.stdout?.on('data', (chunk) => { stdout += chunk })
      proc.stderr?.on('data', (chunk) => { stderr += chunk })
      proc.on('close', (code) => resolve({ code: code || 0, stdout, stderr }))
      proc.on('error', reject)
    })
  }

  private _buildArgs(): string[] {
    return [
      '-y',
      ...this.inputArgs,
      '-i', this.inputPath,
      ...this.outputArgs,
      this.outputPath,
    ].filter(a => a !== '')
  }

  private _spawnProcess(): ReturnType<typeof spawn> {
    const bin = getFFmpegPath()
    const args = this._buildArgs()

    const proc = spawn(bin, args, {
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe'],
    })
    this.proc = proc

    const progressRegex = /frame=\s*(\d+)\s+fps=\s*(\d+)\s+bitrate=\s*([\d.]+)kbits\/s\s+total_size=\s*(\d+)\s+out_time=\s*([\d:.]+)\s+dup=\s*(\d+)\s+drop=\s*(\d+)\s+speed=\s*([\d.]+)x/
    let stderrBuffer = ''

    proc.stderr?.on('data', (chunk: Buffer) => {
      const data = chunk.toString('utf-8')
      stderrBuffer += data

      const lines = stderrBuffer.split(/\r?\n/)
      stderrBuffer = lines.pop() || ''

      for (const line of lines) {
        const match = progressRegex.exec(line)
        if (match) {
          const [, frame, fps, bitrate, totalSize, outTime, dup, drop, speed] = match
          const progress: FFmpegProgress = {
            frame: parseInt(frame, 10),
            fps: parseFloat(fps),
            bitrate: parseFloat(bitrate),
            totalSize: parseInt(totalSize, 10),
            outTime,
            outTimeMs: this._timeToMs(outTime),
            dup: parseInt(dup, 10),
            drop: parseInt(drop, 10),
            speed,
            progress: 'continue',
          }
          this.emit('progress', progress)
        }
        if (line.includes('video:') || line.includes('audio:')) {
          this.emit('progress', { progress: 'end' })
        }
      }
    })

    proc.on('close', (code) => {
      this.proc = null
      if (code === 0) {
        this.emit('end')
      } else {
        const errMsg = `ffmpeg exited with code ${code}`
        this.emit('error', new Error(errMsg))
      }
    })

    proc.on('error', (err) => {
      this.proc = null
      this.emit('error', err)
    })

    return proc
  }

  private _timeToMs(timeStr: string): number {
    const parts = timeStr.split(':').map(Number)
    if (parts.length === 3) return parts[0] * 3600000 + parts[1] * 60000 + parts[2] * 1000
    if (parts.length === 2) return parts[0] * 60000 + parts[1] * 1000
    return parseFloat(timeStr) * 1000
  }

  run(): this {
    this._spawnProcess()
    return this
  }

  kill(signal?: NodeJS.Signals) {
    if (this.proc) {
      this.proc.kill(signal || 'SIGKILL')
      this.proc = null
    }
  }

  get process() { return this.proc }
  get pid() { return this.proc?.pid }
}
