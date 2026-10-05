import { publishDanmaku } from '../../utils/danmaku-events'

export default defineEventHandler(async (event) => {
  try {
    const auth = getAuthFromEvent(event)
    const slug = getRouterParam(event, 'slug')
    if (!slug) throw createError({ statusCode: 400, message: '缺少参数' })

    const body = await readBody(event)
    if (!body || !body.content || !body.content.trim()) {
      throw createError({ statusCode: 400, message: '弹幕内容不能为空' })
    }
    if (body.time === undefined || body.time === null) {
      throw createError({ statusCode: 400, message: '弹幕时间不能为空' })
    }

    const time = parseFloat(body.time)
    const dmType = body.type || 'scroll'
    const dmColor = body.color || '#ffffff'
    const username = auth?.username || '匿名'
    const userId = auth ? parseInt(auth.userId) : null

    const danmaku = await prisma.danmaku.create({
      data: {
        username,
        content: body.content.trim(),
        time,
        type: dmType,
        color: dmColor,
        video: { connect: { slug } },
        ...(userId ? { user: { connect: { id: userId } } } : {}),
      },
    })

    publishDanmaku(slug, {
      type: 'danmaku',
      slug,
      danmaku: {
        id: danmaku.id,
        content: danmaku.content,
        time: danmaku.time,
        type: danmaku.type,
        color: danmaku.color,
        username: danmaku.username,
        userId: danmaku.userId,
      },
    })

    return { success: true, data: danmaku }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Add danmaku error:', err)
    throw createError({ statusCode: 500, message: '发送弹幕失败' })
  }
})
