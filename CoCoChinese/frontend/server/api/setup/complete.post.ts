import { execSync } from 'child_process'
import { readFileSync, writeFileSync } from 'fs'
import { resolve } from 'path'

export default defineEventHandler(async (event) => {
  const body = await readBody(event)
  const envPath = resolve(process.cwd(), '.env')

  try {
    let env = readFileSync(envPath, 'utf-8')
    if (!env.includes('SETUP_COMPLETED')) {
      env += '\nSETUP_COMPLETED="true"\n'
      writeFileSync(envPath, env, 'utf-8')
    }

    try {
      const { queryPg } = await import('../../utils/pg')
      const siteName = body?.siteName || 'Cocokalo'
      await queryPg(
        `INSERT INTO site_configs (key, value) VALUES ('site_name', $1)
         ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value`,
        [siteName]
      )
    } catch (e) {
      console.error('Failed to save site_name:', e)
    }

    try {
      execSync('npx nuxt build', { stdio: 'inherit', cwd: process.cwd(), timeout: 180000 })
    } catch {
      // Build is optional during setup
    }

    return { success: true, message: '安装完成！' }
  } catch (e: any) {
    throw createError({ statusCode: 500, message: `安装完成处理失败: ${e.message}` })
  }
})
