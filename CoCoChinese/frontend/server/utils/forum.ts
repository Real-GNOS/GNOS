import { queryPg } from './pg'

export interface ForumCategory {
  id: number
  name: string
  description: string
  slug: string
  icon: string
  display_order: number
  topic_count: number
  post_count: number
  created_at: string
}

export interface ForumTopic {
  id: number
  title: string
  slug: string
  category_id: number
  category_name?: string
  author_id: number
  author_name?: string
  author_avatar?: string
  content: string
  view_count: number
  reply_count: number
  is_pinned: boolean
  is_locked: boolean
  created_at: string
  updated_at: string
  last_post_at: string
}

export interface ForumPost {
  id: number
  topic_id: number
  author_id: number
  author_name?: string
  author_avatar?: string
  content: string
  created_at: string
  updated_at: string
}

export async function getCategories(): Promise<ForumCategory[]> {
  const result = await queryPg(
    `SELECT fc.*, 
       COALESCE((SELECT count(*) FROM forum_topics ft WHERE ft.category_id = fc.id), 0) as topic_count,
       COALESCE((SELECT count(*) FROM forum_posts fp JOIN forum_topics ft ON ft.id = fp.topic_id WHERE ft.category_id = fc.id), 0) as post_count
     FROM forum_categories fc
     ORDER BY fc.display_order ASC, fc.id ASC`
  )
  return result.rows
}

export async function getCategoryBySlug(slug: string): Promise<ForumCategory | null> {
  const result = await queryPg(
    `SELECT fc.*,
       COALESCE((SELECT count(*) FROM forum_topics ft WHERE ft.category_id = fc.id), 0) as topic_count,
       COALESCE((SELECT count(*) FROM forum_posts fp JOIN forum_topics ft ON ft.id = fp.topic_id WHERE ft.category_id = fc.id), 0) as post_count
     FROM forum_categories fc WHERE fc.slug = $1`, [slug]
  )
  return result.rows[0] || null
}

export async function getTopics(categorySlug?: string, page = 1, pageSize = 20): Promise<{ topics: ForumTopic[]; total: number }> {
  const offset = (page - 1) * pageSize
  let where = ''
  const params: any[] = []
  if (categorySlug) {
    where = 'WHERE fc.slug = $1'
    params.push(categorySlug)
  }
  const countResult = await queryPg(
    `SELECT count(*) FROM forum_topics ft LEFT JOIN forum_categories fc ON fc.id = ft.category_id ${where}`, params
  )
  const total = parseInt(countResult.rows[0].count)

  const topicParams: any[] = []
  let topicWhere = ''
  if (categorySlug) {
    topicWhere = 'WHERE fc.slug = $1'
    topicParams.push(categorySlug)
  }
  topicParams.push(pageSize, offset)
  const result = await queryPg(
    `SELECT ft.*, fc.name as category_name,
       u.username as author_name, u.avatar_url as author_avatar
     FROM forum_topics ft
     LEFT JOIN forum_categories fc ON fc.id = ft.category_id
     LEFT JOIN users u ON u.id = ft.author_id
     ${topicWhere}
     ORDER BY ft.is_pinned DESC, ft.last_post_at DESC
     LIMIT $${topicParams.length - 1} OFFSET $${topicParams.length}`, topicParams
  )
  return { topics: result.rows, total }
}

export async function getTopicBySlug(slug: string): Promise<ForumTopic & { category_slug?: string } | null> {
  const result = await queryPg(
    `SELECT ft.*, fc.name as category_name, fc.slug as category_slug,
       u.username as author_name, u.avatar_url as author_avatar
     FROM forum_topics ft
     LEFT JOIN forum_categories fc ON fc.id = ft.category_id
     LEFT JOIN users u ON u.id = ft.author_id
     WHERE ft.slug = $1`, [slug]
  )
  return result.rows[0] || null
}

export async function getForumPosts(topicSlug: string, page = 1, pageSize = 20): Promise<{ posts: ForumPost[]; total: number }> {
  const offset = (page - 1) * pageSize
  const countResult = await queryPg(
    `SELECT count(*) FROM forum_posts fp JOIN forum_topics ft ON ft.id = fp.topic_id WHERE ft.slug = $1`, [topicSlug]
  )
  const total = parseInt(countResult.rows[0].count)

  const result = await queryPg(
    `SELECT fp.*, u.username as author_name, u.avatar_url as author_avatar
     FROM forum_posts fp
     JOIN forum_topics ft ON ft.id = fp.topic_id
     LEFT JOIN users u ON u.id = fp.author_id
     WHERE ft.slug = $1
     ORDER BY fp.created_at ASC
     LIMIT $2 OFFSET $3`, [topicSlug, pageSize, offset]
  )
  return { posts: result.rows, total }
}

export async function incrementTopicView(slug: string) {
  await queryPg('UPDATE forum_topics SET view_count = view_count + 1 WHERE slug = $1', [slug])
}

export async function createTopic(data: {
  title: string
  slug: string
  category_id: number
  author_id: number
  content: string
}): Promise<ForumTopic> {
  const result = await queryPg(
    `INSERT INTO forum_topics (title, slug, category_id, author_id, content)
     VALUES ($1, $2, $3, $4, $5) RETURNING *`,
    [data.title, data.slug, data.category_id, data.author_id, data.content]
  )
  await queryPg(
    'UPDATE forum_categories SET topic_count = topic_count + 1 WHERE id = $1', [data.category_id]
  )
  return result.rows[0]
}

export async function createForumPost(data: {
  topic_id: number
  author_id: number
  content: string
}): Promise<ForumPost> {
  const result = await queryPg(
    `INSERT INTO forum_posts (topic_id, author_id, content)
     VALUES ($1, $2, $3) RETURNING *`,
    [data.topic_id, data.author_id, data.content]
  )
  await queryPg(
    'UPDATE forum_topics SET reply_count = reply_count + 1, last_post_at = CURRENT_TIMESTAMP WHERE id = $1',
    [data.topic_id]
  )
  await queryPg(
    `UPDATE forum_categories SET post_count = post_count + 1
     WHERE id = (SELECT category_id FROM forum_topics WHERE id = $1)`, [data.topic_id]
  )
  return result.rows[0]
}

export async function generateUniqueSlug(table: string): Promise<string> {
  const chars = 'abcdefghijklmnopqrstuvwxyz0123456789'
  const { randomBytes } = await import('crypto')
  for (let i = 0; i < 10; i++) {
    const bytes = randomBytes(8)
    let slug = ''
    for (let j = 0; j < 8; j++) {
      slug += chars[bytes[j] % chars.length]
    }
    const exists = await queryPg(`SELECT 1 FROM "${table}" WHERE slug = $1`, [slug])
    if (!exists.rows.length) return slug
  }
  throw new Error('Failed to generate unique slug')
}
