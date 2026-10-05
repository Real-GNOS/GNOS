export default defineEventHandler(async (event) => {
  try {
    const slug = getRouterParam(event, 'id')
    if (!slug) {
      throw createError({ statusCode: 400, message: '缺少媒体ID' })
    }
    const v = await prisma.video.findUnique({ where: { slug } })
    if (!v) {
      throw createError({ statusCode: 404, message: '未找到该媒体文件' })
    }

    const query = getQuery(event)
    const sessionId = query.sessionId as string
    let encryptUrl: ((s: string) => string) | null = null
    if (sessionId) {
      const key = getSessionKey(sessionId)
      if (key) {
        encryptUrl = (url: string) => url ? aesEncrypt(key, url) : ''
      }
    }

    const data: any = {
      id: v.id,
      title: v.title,
      author: v.author,
      authorImg: v.authorImg,
      imageUrl: v.imageUrl,
      videoType: v.videoType,
      videoTime: v.videoTime,
      watchVolue: v.watchVolue,
      likeVolue: v.likeVolue,
      watchPeople: v.watchPeople,
      recommend: v.recommend,
      introduction: v.introduction,
      description: v.description,
    }

    if (encryptUrl) {
      data.videoUrl = encryptUrl(v.videoUrl)
      data.videoUrlHls = encryptUrl(v.videoUrlHls || '')
      data.videoUrl360p = encryptUrl(v.videoUrl360p || '')
      data.videoUrl720p = encryptUrl(v.videoUrl720p || '')
      data.videoUrl1080p = encryptUrl(v.videoUrl1080p || '')
      data._enc = true
    } else {
      data.videoUrl = v.videoUrl
      data.videoUrlHls = v.videoUrlHls || ''
      data.videoUrl360p = v.videoUrl360p || ''
      data.videoUrl720p = v.videoUrl720p || ''
      data.videoUrl1080p = v.videoUrl1080p || ''
    }

    return data
  } catch (err) {
    if (err.statusCode) throw err
    console.error(err)
    throw createError({ statusCode: 500, message: '服务器错误' })
  }
})
