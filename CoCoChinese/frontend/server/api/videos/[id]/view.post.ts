export default defineEventHandler(async (event) => {
  try {
    const id = parseInt(getRouterParam(event, 'id') || '')
    if (isNaN(id)) throw createError({ statusCode: 400, message: '无效ID' })

    const video = await queryPg('SELECT id, watch_volue FROM videos WHERE id = $1', [id])
    if (!video.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })

    const newVal = parseInt(video.rows[0].watch_volue || '0') + 1
    await queryPg('UPDATE videos SET watch_volue = $1 WHERE id = $2', [String(newVal), id])
    return { watchVolue: newVal }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('View count error:', err)
    throw createError({ statusCode: 500, message: '记录失败' })
  }
})
