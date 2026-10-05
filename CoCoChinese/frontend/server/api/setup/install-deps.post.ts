import { execSync } from 'child_process'
import { detectPackageManager } from '../../utils/installer'

export default defineEventHandler(async () => {
  try {
    const pm = detectPackageManager()
    execSync(`${pm} install`, { stdio: 'inherit', cwd: process.cwd(), timeout: 300000 })
    return { success: true, message: '依赖安装完成' }
  } catch (e: any) {
    throw createError({ statusCode: 500, message: `依赖安装失败: ${e.message}` })
  }
})
