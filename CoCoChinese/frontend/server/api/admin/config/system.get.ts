import { execSync } from 'child_process'
import { existsSync } from 'fs'
import { resolve } from 'path'
import { checkCommand } from '../../../utils/installer'

export default defineEventHandler(async (event) => {
  await requireAdmin(event)

  const system = {
    node: process.version,
    platform: process.platform,
    arch: process.arch,
    memory: process.memoryUsage(),
    uptime: process.uptime(),
    env: process.env.NODE_ENV || 'development',
    tools: {
      ffmpeg: checkCommand('ffmpeg'),
      ffprobe: checkCommand('ffprobe'),
      git: checkCommand('git'),
      psql: checkCommand('psql'),
      redis: checkCommand('redis-cli'),
    },
    storage: {
      total: 0,
      used: 0,
      free: 0,
    },
  }

  try {
    const df = execSync('df -k . | tail -1').toString().trim().split(/\s+/)
    system.storage = {
      total: parseInt(df[1]) * 1024 || 0,
      used: parseInt(df[2]) * 1024 || 0,
      free: parseInt(df[3]) * 1024 || 0,
    }
  } catch {}

  return system
})
