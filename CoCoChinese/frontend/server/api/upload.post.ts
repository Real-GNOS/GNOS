import { writeFile, mkdir } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const body = await readMultipartFormData(event)
    if (!body) throw createError({ statusCode: 400, message: '没有数据' })

    const titleField = body.find(f => f.name === 'title')
    const descField = body.find(f => f.name === 'description')
    const catField = body.find(f => f.name === 'category')
    const tagsField = body.find(f => f.name === 'tags')
    const fileField = body.find(f => f.name === 'file')
    const coverField = body.find(f => f.name === 'cover')

    if (!titleField || !titleField.data.toString().trim()) {
      throw createError({ statusCode: 400, message: '标题不能为空' })
    }
    if (!fileField) {
      throw createError({ statusCode: 400, message: '请选择视频文件' })
    }

    const title = titleField.data.toString().trim()
    const description = descField?.data.toString().trim() || ''
    const category = catField?.data.toString().trim() || ''
    const tagsStr = tagsField?.data.toString().trim() || ''
    const tags = tagsStr ? tagsStr.split(',').map(t => t.trim()).filter(Boolean) : []

    const slug = await uniqueSlug('videos')
    const uploadDir = path.resolve('storage/videos')

    await mkdir(uploadDir, { recursive: true })

    const rawFile = path.join(uploadDir, `${slug}_raw${path.extname(fileField.filename || '.mp4') || '.mp4'}`)
    await writeFile(rawFile, fileField.data)

    const finalFile = path.join(uploadDir, `${slug}.mp4`)

    const { processVideo, transcodeAllResolutions } = await import('~/server/utils/media')
    const videoTime = await processVideo(rawFile, finalFile)

    const videoUrl = `/videos/${slug}.mp4`

    let videoUrlHls = ''
    let videoUrl360p = ''
    let videoUrl720p = ''
    let videoUrl1080p = ''
    try {
      const hlsBaseDir = path.resolve('storage/videos')
      const result = await transcodeAllResolutions(finalFile, hlsBaseDir, slug)
      videoUrl360p = result.videoUrl360p
      videoUrl720p = result.videoUrl720p
      videoUrl1080p = result.videoUrl1080p
      videoUrlHls = result.videoUrlHls
    } catch (hlsErr) {
      console.error('Multi-resolution transcoding failed (non-fatal):', hlsErr)
    }

    try {
      const { transcodeAllFMP4 } = await import('~/server/utils/fmp4')
      const fmp4BaseDir = path.resolve('storage/fmp4')
      await transcodeAllFMP4(finalFile, fmp4BaseDir, slug)
    } catch (fmp4Err) {
      console.error('fMP4 transcoding failed (non-fatal):', fmp4Err)
    }

    let imageUrl = ''
    if (coverField && coverField.data && coverField.data.length > 0) {
      const coverExt = path.extname(coverField.filename || '.webp') || '.webp'
      const coverName = `${slug}_cover${coverExt}`
      const imagesDir = path.resolve('public/images')
      await mkdir(imagesDir, { recursive: true })
      await writeFile(path.join(imagesDir, coverName), coverField.data)
      imageUrl = `/images/${coverName}`
    }

    await queryPg(
      `INSERT INTO videos (slug, title, author, author_img, introduction, video_type, video_url, video_url_hls, image_url, tags, video_time, "order")
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, 0)`,
      [slug, title, auth.username || '匿名', auth.avatar_url || '/images/authorImg.webp', description, category, videoUrl, videoUrlHls, imageUrl, tags, videoTime]
    )

    await queryPg(
      `UPDATE videos SET video_url_360p = $1, video_url_720p = $2, video_url_1080p = $3 WHERE slug = $4`,
      [videoUrl360p, videoUrl720p, videoUrl1080p, slug]
    )

    return { success: true, slug, video_time: videoTime, message: '投稿成功' }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Upload error:', err)
    throw createError({ statusCode: 500, message: '投稿失败' })
  }
})
