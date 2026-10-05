import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import fs from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id') || '')

  if (!id) throw createError({ statusCode: 400, message: '无效ID' })

  if (event.method === 'DELETE') {
    const file = await queryPg('SELECT * FROM media_files WHERE id = $1', [id])
    if (!file.rows.length) throw createError({ statusCode: 404, message: '文件不存在' })

    const filePath = file.rows[0].file_path
    const absolutePath = path.resolve(process.cwd(), 'public', filePath.replace(/^\//, ''))

    try {
      await fs.unlink(absolutePath)
    } catch {}

    await queryPg('DELETE FROM media_files WHERE id = $1', [id])

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'delete_file', 'media_file', id, `删除文件: ${file.rows[0].name || filePath}`]
    )

    return { success: true }
  }

  throw createError({ statusCode: 405 })
})
