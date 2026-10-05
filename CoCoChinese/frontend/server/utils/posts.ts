export interface Post {
  id: number
  slug: string
  title: string
  content: string
  type: string
  author: string
  author_img: string
  cover_image: string
  images: string[]
  category: string
  tags: string[]
  status: string
  view_count: number
  like_count: number
  created_at: string
  updated_at: string
}

function toLegacyPost(row: any): Post {
  return {
    id: row.id,
    slug: row.slug,
    title: row.title,
    content: row.content,
    type: row.type,
    author: row.author,
    author_img: row.author_img,
    cover_image: row.cover_image,
    images: typeof row.images === 'string' ? JSON.parse(row.images) : (row.images || []),
    category: row.category || '',
    tags: Array.isArray(row.tags) ? row.tags : [],
    status: row.status,
    view_count: row.view_count,
    like_count: row.like_count,
    created_at: row.created_at,
    updated_at: row.updated_at,
  }
}

export async function getPosts(options?: { type?: string; author?: string; limit?: number; offset?: number }) {
  const conditions: string[] = ['1=1']
  const params: any[] = []
  let idx = 1
  if (options?.type) {
    conditions.push(`type = $${idx++}`)
    params.push(options.type)
  }
  if (options?.author) {
    conditions.push(`author = $${idx++}`)
    params.push(options.author)
  }
  conditions.push(`status = 'published'`)
  const limit = options?.limit || 20
  const offset = options?.offset || 0
  params.push(limit, offset)
  const result = await queryPg(
    `SELECT * FROM posts WHERE ${conditions.join(' AND ')} ORDER BY created_at DESC LIMIT $${idx++} OFFSET $${idx}`,
    params
  )
  return result.rows.map(toLegacyPost)
}

export async function getPostBySlug(slug: string) {
  const result = await queryPg('SELECT * FROM posts WHERE slug = $1', [slug])
  return result.rows.length ? toLegacyPost(result.rows[0]) : null
}

export async function createPost(data: {
  slug: string; title: string; content?: string; type?: string
  author: string; author_img?: string; cover_image?: string
  category?: string; tags?: string[]; status?: string; images?: string[]
}) {
  const result = await queryPg(
    `INSERT INTO posts (slug, title, content, type, author, author_img, cover_image, category, tags, status, images)
     VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11) RETURNING *`,
    [data.slug, data.title, data.content || '', data.type || 'article',
     data.author, data.author_img || '', data.cover_image || '',
     data.category || '', data.tags || [], data.status || 'published',
     data.images ? JSON.stringify(data.images) : '[]']
  )
  return toLegacyPost(result.rows[0])
}

export async function updatePost(slug: string, data: {
  title?: string; content?: string; cover_image?: string
  category?: string; tags?: string[]; status?: string; images?: string[]
}) {
  const sets: string[] = []
  const params: any[] = []
  let idx = 1
  if (data.title !== undefined) { sets.push(`title = $${idx++}`); params.push(data.title) }
  if (data.content !== undefined) { sets.push(`content = $${idx++}`); params.push(data.content) }
  if (data.cover_image !== undefined) { sets.push(`cover_image = $${idx++}`); params.push(data.cover_image) }
  if (data.category !== undefined) { sets.push(`category = $${idx++}`); params.push(data.category) }
  if (data.tags !== undefined) { sets.push(`tags = $${idx++}`); params.push(data.tags) }
  if (data.status !== undefined) { sets.push(`status = $${idx++}`); params.push(data.status) }
  if (data.images !== undefined) { sets.push(`images = $${idx++}`); params.push(JSON.stringify(data.images)) }
  if (!sets.length) return null
  sets.push(`updated_at = CURRENT_TIMESTAMP`)
  params.push(slug)
  const result = await queryPg(
    `UPDATE posts SET ${sets.join(', ')} WHERE slug = $${idx} RETURNING *`,
    params
  )
  return result.rows.length ? toLegacyPost(result.rows[0]) : null
}

export async function deletePost(slug: string) {
  await queryPg('DELETE FROM posts WHERE slug = $1', [slug])
}

export async function incrementPostViews(slug: string) {
  await queryPg('UPDATE posts SET view_count = view_count + 1 WHERE slug = $1', [slug])
}
