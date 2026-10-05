import { execSync } from 'child_process'

export default defineEventHandler(async () => {
  try {
    execSync('npx prisma generate', { stdio: 'inherit', cwd: process.cwd(), timeout: 120000 })
    return { success: true, message: 'Prisma 客户端已生成' }
  } catch (e: any) {
    throw createError({ statusCode: 500, message: `Prisma 生成失败: ${e.message}` })
  }
})
