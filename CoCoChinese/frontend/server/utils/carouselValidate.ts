import { createError } from 'h3'

/*
 * Server-side validation for carousel create/update payloads.
 * Mirrors the limits enforced in pages/admin/carousel.vue, but lives here
 * because the REST endpoints are reachable without going through the UI --
 * anything the admin API accepts must be safe on its own.
 */

export interface CarouselInput {
  image_url?: unknown
  link?: unknown
  title?: unknown
  author?: unknown
  author_img?: unknown
  watch_volue?: unknown
  like_volue?: unknown
  introduction?: unknown
  video_time?: unknown
  description?: unknown
  order?: unknown
}

const VIDEO_TIME_RE = /^\d{1,3}(:\d{1,2}){0,2}$/

function bad(message: string): never {
  throw createError({ statusCode: 400, message })
}

function str(v: unknown, field: string, max: number, required = false): string {
  const s = typeof v === 'string' ? v : v == null ? '' : String(v)
  if (required && !s.trim()) bad(`${field}不能为空`)
  if (s.length > max) bad(`${field}过长（≤${max} 字）`)
  return s
}

function count(v: unknown, field: string): string {
  // columns are varchar; accept numbers or digit strings, reject everything else
  if (v === undefined || v === null || v === '') return '0'
  const n = typeof v === 'number' ? v : Number(v)
  if (!Number.isInteger(n) || n < 0 || n > 1e9) bad(`${field}必须是不小于 0 的整数`)
  return String(n)
}

/** Validates fields present on `body`; returns normalized values for them. */
export function sanitizeCarousel(body: CarouselInput, { requireImage = false } = {}):
    Partial<Record<keyof CarouselInput, string | number>> {
  if (!body || typeof body !== 'object') bad('请求体无效')

  const out: Partial<Record<keyof CarouselInput, string | number>> = {}

  if (body.title !== undefined) out.title = str(body.title, '标题', 200, true)
  else if (requireImage) bad('标题不能为空')

  if (body.author !== undefined) out.author = str(body.author, '作者名', 100)
  if (body.author_img !== undefined) out.author_img = str(body.author_img, '作者头像', 500)

  if (body.image_url !== undefined) out.image_url = str(body.image_url, '图片', 500)
  else if (requireImage) bad('请上传轮播图图片')

  if (body.link !== undefined) out.link = str(body.link, '链接', 500)

  if (body.watch_volue !== undefined) out.watch_volue = count(body.watch_volue, '播放量')
  if (body.like_volue !== undefined) out.like_volue = count(body.like_volue, '点赞量')

  if (body.introduction !== undefined) out.introduction = str(body.introduction, '简介', 500)
  if (body.description !== undefined) out.description = str(body.description, '描述', 1000)

  if (body.video_time !== undefined) {
    const t = str(body.video_time, '视频时长', 20)
    if (t.trim() && !VIDEO_TIME_RE.test(t.trim())) bad('视频时长格式应为 分:秒 或 时:分:秒，例如 24:00')
    out.video_time = t.trim()
  }

  if (body.order !== undefined) {
    const n = typeof body.order === 'number' ? body.order : Number(body.order)
    if (!Number.isInteger(n) || n < 0) bad('排序值必须是大于等于 0 的整数')
    out.order = n
  }

  return out
}
