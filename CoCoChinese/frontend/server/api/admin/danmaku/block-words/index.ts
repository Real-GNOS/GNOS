import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'GET') {
    const result = await queryPg("SELECT value FROM site_configs WHERE key = 'danmaku_block_words'")
    const blockWords: string[] = result.rows.length
      ? JSON.parse(result.rows[0].value)
      : []
    return { success: true, blockWords }
  }

  if (event.method === 'POST') {
    const body = await readBody(event)
    const { word } = body

    if (!word || !word.trim()) {
      throw createError({ statusCode: 400, message: '屏蔽词不能为空' })
    }

    const result = await queryPg("SELECT value FROM site_configs WHERE key = 'danmaku_block_words'")
    let blockWords: string[] = result.rows.length
      ? JSON.parse(result.rows[0].value)
      : []

    if (blockWords.includes(word.trim())) {
      return { success: true, message: '该词已存在', blockWords }
    }

    blockWords.push(word.trim())

    await queryPg(`
      INSERT INTO site_configs (key, value) VALUES ('danmaku_block_words', $1)
      ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value
    `, [JSON.stringify(blockWords)])

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, details) VALUES ($1, $2, $3, $4)',
      [auth.userId, 'add_block_word', 'danmaku', `添加弹幕屏蔽词: ${word.trim()}`]
    )

    return { success: true, blockWords }
  }

  if (event.method === 'DELETE') {
    const query = getQuery(event)
    const word = query.word as string

    if (!word) throw createError({ statusCode: 400, message: '缺少参数' })

    const result = await queryPg("SELECT value FROM site_configs WHERE key = 'danmaku_block_words'")
    let blockWords: string[] = result.rows.length
      ? JSON.parse(result.rows[0].value)
      : []

    blockWords = blockWords.filter(w => w !== word)

    await queryPg(`
      INSERT INTO site_configs (key, value) VALUES ('danmaku_block_words', $1)
      ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value
    `, [JSON.stringify(blockWords)])

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, details) VALUES ($1, $2, $3, $4)',
      [auth.userId, 'remove_block_word', 'danmaku', `移除弹幕屏蔽词: ${word}`]
    )

    return { success: true, blockWords }
  }

  throw createError({ statusCode: 405 })
})
