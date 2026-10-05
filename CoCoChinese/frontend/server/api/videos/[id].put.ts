import { writeFile, mkdir, unlink } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const id = getRouterParam(event, 'id')

    const existing = await queryPg('SELECT * FROM videos WHERE id = $1', [id])
    if (!existing.rows.length) {
      throw createError({ statusCode: 404, message: '视频不存在' })
    }

    const video = existing.rows[0]
    if (video.author !== auth.username) {
      throw createError({ statusCode: 403, message: '只能编辑自己的视频' })
    }

    const contentType = getHeader(event, 'content-type') || ''
    let title = video.title
    let introduction = video.introduction
    let category = video.video_type
    let tags = Array.isArray(video.tags) ? video.tags : (video.tags ? [video.tags] : [])
    let imageUrl = video.image_url
    let videoUrl = video.video_url
    let videoTime = video.video_time

    if (contentType.includes('multipart/form-data')) {
      const body = await readMultipartFormData(event)
      if (!body) throw createError({ statusCode: 400, message: '没有数据' })

      const titleField = body.find(f => f.name === 'title')
      const descField = body.find(f => f.name === 'description')
      const catField = body.find(f => f.name === 'category')
      const tagsField = body.find(f => f.name === 'tags')
      const coverField = body.find(f => f.name === 'cover')
      const fileField = body.find(f => f.name === 'file')

      if (titleField && titleField.data.toString().trim()) {
        title = titleField.data.toString().trim()
      }
      introduction = descField?.data.toString().trim() || introduction
      category = catField?.data.toString().trim() || category
      const tagsStr = tagsField?.data.toString().trim() || ''
      if (tagsStr) {
        tags = tagsStr.split(',').map(t => t.trim()).filter(Boolean)
      }

      if (coverField && coverField.data && coverField.data.length > 0) {
        const coverExt = path.extname(coverField.filename || '.webp') || '.webp'
        const coverName = `${video.slug}_cover${coverExt}`
        const imagesDir = path.resolve('public/images')
        await mkdir(imagesDir, { recursive: true })
        await writeFile(path.join(imagesDir, coverName), coverField.data)
        imageUrl = `/images/${coverName}`
      }

      if (fileField && fileField.data && fileField.data.length > 0) {
        const uploadDir = path.resolve('storage/videos')
        await mkdir(uploadDir, { recursive: true })
        if (video.video_url) {
          const oldPath = path.join(uploadDir, path.basename(video.video_url))
          try { await unlink(oldPath) } catch {}
        }
        const rawFile = path.join(uploadDir, `${video.slug}_raw${path.extname(fileField.filename || '.mp4') || '.mp4'}`)
        await writeFile(rawFile, fileField.data)
        const finalFile = path.join(uploadDir, `${video.slug}.mp4`)
        const { processVideo } = await import('~/server/utils/media')
        videoTime = await processVideo(rawFile, finalFile)
        videoUrl = `/videos/${video.slug}.mp4`

        try {
          const { transcodeAllFMP4 } = await import('~/server/utils/fmp4')
          const fmp4BaseDir = path.resolve('storage/fmp4')
          await transcodeAllFMP4(finalFile, fmp4BaseDir, video.slug)
        } catch (fmp4Err) {
          console.error('fMP4 transcoding failed (non-fatal):', fmp4Err)
        }
      }
    } else {
      const body = await readBody(event)
      if (!body) throw createError({ statusCode: 400, message: '没有数据' })
      if (body.title?.trim()) title = body.title.trim()
      if (body.description !== undefined) introduction = body.description.trim()
      if (body.category) category = body.category
      if (body.tags) tags = body.tags.split(',').map(t => t.trim()).filter(Boolean)
    }

    await queryPg(
      `UPDATE videos SET title = $1, introduction = $2, video_type = $3, tags = $4, image_url = $5, video_url = $6, video_time = $7, updated_at = CURRENT_TIMESTAMP WHERE id = $8`,
      [title, introduction, category, tags, imageUrl, videoUrl, videoTime, id]
    )

    return { success: true, message: '更新成功' }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Edit error:', err)
    throw createError({ statusCode: 500, message: '更新失败' })
  }
})