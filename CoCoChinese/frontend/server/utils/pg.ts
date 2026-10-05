import pg from 'pg'
import bcrypt from 'bcryptjs'
import { randomBytes } from 'crypto'

let pool: pg.Pool | null = null

export function generateSlug(length = 8) {
  const chars = 'abcdefghijklmnopqrstuvwxyz0123456789'
  let slug = ''
  const bytes = randomBytes(length)
  for (let i = 0; i < length; i++) {
    slug += chars[bytes[i] % chars.length]
  }
  return slug
}

export async function uniqueSlug(table: string, column = 'slug', length = 8) {
  for (let i = 0; i < 10; i++) {
    const slug = generateSlug(length)
    const exists = await queryPg(`SELECT 1 FROM "${table}" WHERE "${column}" = $1`, [slug])
    if (!exists.rows.length) return slug
  }
  throw new Error('Failed to generate unique slug')
}

const DB_NAME = process.env.PG_DB || 'cocokalo'

function makePool(dbName: string) {
  return new pg.Pool({
    host: process.env.PG_HOST || 'localhost',
    port: parseInt(process.env.PG_PORT || '5432'),
    database: dbName,
    user: process.env.PG_USER || 'postgres',
    password: String(process.env.PG_PASSWORD || '123'),
    max: 10,
    idleTimeoutMillis: 30000,
  })
}

export function getPgPool() {
  if (!pool) {
    pool = makePool(DB_NAME)
  }
  return pool
}

export async function queryPg(text: string, params?: any[]) {
  const client = await getPgPool().connect()
  try {
    const result = await client.query(text, params)
    return result
  } finally {
    client.release()
  }
}

export async function ensureDatabase() {
  const tempPool = makePool('postgres')
  const client = await tempPool.connect()
  try {
    const res = await client.query(
      `SELECT 1 FROM pg_database WHERE datname = $1`, [DB_NAME]
    )
    if (!res.rows.length) {
      await client.query(`CREATE DATABASE "${DB_NAME}"`)
      console.log(`Database "${DB_NAME}" created`)
    }
  } finally {
    client.release()
    await tempPool.end()
  }
  pool = makePool(DB_NAME)
}

export async function initPgTables() {
  await ensureDatabase()
  const client = await getPgPool().connect()
  try {
    await client.query(`
      CREATE TABLE IF NOT EXISTS carousels (
        id SERIAL PRIMARY KEY,
        image_url VARCHAR(500),
        link VARCHAR(500),
        title VARCHAR(255),
        author VARCHAR(100),
        author_img VARCHAR(500),
        watch_volue VARCHAR(20) DEFAULT '0',
        like_volue VARCHAR(20) DEFAULT '0',
        introduction TEXT,
        video_time VARCHAR(20),
        description TEXT,
        "order" INTEGER DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS notices (
        id SERIAL PRIMARY KEY,
        title VARCHAR(255) NOT NULL,
        content TEXT,
        link VARCHAR(500),
        is_active BOOLEAN DEFAULT true,
        "order" INTEGER DEFAULT 0,
        created_by INTEGER,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS media_files (
        id SERIAL PRIMARY KEY,
        file_id VARCHAR(64) UNIQUE,
        file_path VARCHAR(500) NOT NULL,
        name VARCHAR(255),
        file_type VARCHAR(50),
        file_size BIGINT,
        file_hash VARCHAR(128),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS users (
        id SERIAL PRIMARY KEY,
        slug VARCHAR(8) UNIQUE,
        username VARCHAR(50) UNIQUE NOT NULL,
        password VARCHAR(255) NOT NULL,
        email VARCHAR(255) UNIQUE,
        role VARCHAR(20) DEFAULT 'user',
        avatar_url VARCHAR(500),
        display_name VARCHAR(100) DEFAULT '',
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS videos (
        id SERIAL PRIMARY KEY,
        slug VARCHAR(8) UNIQUE,
        title VARCHAR(255) NOT NULL,
        description TEXT,
        author VARCHAR(100),
        author_img VARCHAR(500),
        image_url VARCHAR(500),
        video_url VARCHAR(500),
        video_type VARCHAR(50),
        video_time VARCHAR(20),
        watch_volue VARCHAR(20) DEFAULT '0',
        like_volue VARCHAR(20) DEFAULT '0',
        watch_people VARCHAR(20) DEFAULT '0',
        recommend VARCHAR(100),
        introduction TEXT,
        category VARCHAR(50),
        tags TEXT[],
        "order" INTEGER DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS posts (
        id SERIAL PRIMARY KEY,
        slug VARCHAR(50) UNIQUE,
        title VARCHAR(255) NOT NULL,
        content TEXT DEFAULT '',
        type VARCHAR(20) DEFAULT 'article',
        author VARCHAR(100),
        author_img VARCHAR(500),
        cover_image VARCHAR(500),
        images JSONB DEFAULT '[]',
        category VARCHAR(50),
        tags TEXT[],
        status VARCHAR(20) DEFAULT 'published',
        view_count INTEGER DEFAULT 0,
        like_count INTEGER DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS categories (
        id SERIAL PRIMARY KEY,
        name VARCHAR(100) NOT NULL,
        icon VARCHAR(100),
        "order" INTEGER DEFAULT 0,
        parent_id INTEGER REFERENCES categories(id),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS user_profiles (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        bio TEXT DEFAULT '',
        gender VARCHAR(10) DEFAULT '',
        birthday DATE,
        "location" VARCHAR(100) DEFAULT '',
        website VARCHAR(500) DEFAULT '',
        followers INTEGER DEFAULT 0,
        "following" INTEGER DEFAULT 0,
        likes INTEGER DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS favorite_folders (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        name VARCHAR(100) NOT NULL,
        description TEXT DEFAULT '',
        is_public BOOLEAN DEFAULT true,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS favorites (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        folder_id INTEGER REFERENCES favorite_folders(id) ON DELETE CASCADE,
        video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS watch_history (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,
        progress FLOAT DEFAULT 0,
        watched_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS messages (
        id SERIAL PRIMARY KEY,
        from_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        to_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        content TEXT NOT NULL,
        is_read BOOLEAN DEFAULT false,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS activities (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        type VARCHAR(50) NOT NULL,
        target_id INTEGER,
        content TEXT DEFAULT '',
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS comments (
        id SERIAL PRIMARY KEY,
        video_slug VARCHAR(8) NOT NULL REFERENCES videos(slug) ON DELETE CASCADE,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        username VARCHAR(50) NOT NULL,
        avatar_url VARCHAR(500),
        content TEXT NOT NULL,
        parent_id INTEGER REFERENCES comments(id) ON DELETE CASCADE,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS danmaku (
        id SERIAL PRIMARY KEY,
        video_slug VARCHAR(8) NOT NULL REFERENCES videos(slug) ON DELETE CASCADE,
        user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
        username VARCHAR(50) DEFAULT '匿名',
        content TEXT NOT NULL,
        time FLOAT NOT NULL,
        type VARCHAR(20) DEFAULT 'scroll',
        color VARCHAR(7) DEFAULT '#ffffff',
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS video_likes (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(user_id, video_id)
      );

      CREATE TABLE IF NOT EXISTS user_follows (
        id SERIAL PRIMARY KEY,
        follower_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        following_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(follower_id, following_id)
      );

      CREATE TABLE IF NOT EXISTS user_bans (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        operator_id INTEGER NOT NULL REFERENCES users(id) ON DELETE SET NULL,
        type VARCHAR(20) NOT NULL DEFAULT 'ban',
        reason TEXT NOT NULL,
        duration INTEGER,
        expires_at TIMESTAMP,
        is_active BOOLEAN DEFAULT true,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS notifications (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        type VARCHAR(50) NOT NULL DEFAULT 'system',
        title VARCHAR(255) NOT NULL DEFAULT '',
        body TEXT DEFAULT '',
        from_user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
        is_read BOOLEAN DEFAULT false,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS audit_logs (
        id SERIAL PRIMARY KEY,
        user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
        action VARCHAR(100) NOT NULL,
        target_type VARCHAR(50),
        target_id VARCHAR(50),
        details TEXT,
        ip_address VARCHAR(45),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS content_reports (
        id SERIAL PRIMARY KEY,
        reporter_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        target_type VARCHAR(50) NOT NULL,
        target_id VARCHAR(50) NOT NULL,
        reason TEXT NOT NULL,
        description TEXT DEFAULT '',
        status VARCHAR(20) DEFAULT 'pending',
        handled_by_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
        handled_at TIMESTAMP,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS video_versions (
        id SERIAL PRIMARY KEY,
        video_slug VARCHAR(8) NOT NULL REFERENCES videos(slug) ON DELETE CASCADE,
        version INTEGER NOT NULL,
        commit_sha VARCHAR(40) NOT NULL,
        tag VARCHAR(20),
        message TEXT DEFAULT '',
        operator_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
        operator_name VARCHAR(50),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(video_slug, version)
      );

      CREATE TABLE IF NOT EXISTS article_versions (
        id SERIAL PRIMARY KEY,
        article_slug VARCHAR(50) NOT NULL REFERENCES posts(slug) ON DELETE CASCADE,
        version INTEGER NOT NULL,
        commit_sha VARCHAR(40) NOT NULL,
        tag VARCHAR(20),
        message TEXT DEFAULT '',
        operator_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
        operator_name VARCHAR(50),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(article_slug, version)
      );
    `)

    await client.query(`CREATE EXTENSION IF NOT EXISTS pg_trgm`)

    await client.query(`ALTER TABLE videos ADD COLUMN IF NOT EXISTS tsv tsvector`)
    await client.query(`
      CREATE INDEX IF NOT EXISTS idx_videos_tsv ON videos USING GIN(tsv)
    `)
    await client.query(`
      CREATE OR REPLACE FUNCTION videos_tsv_update() RETURNS trigger AS $$
      BEGIN
        NEW.tsv :=
          setweight(to_tsvector('simple', coalesce(NEW.title, '')), 'A') ||
          setweight(to_tsvector('simple', coalesce(array_to_string(NEW.tags, ' '), '')), 'B') ||
          setweight(to_tsvector('simple', coalesce(NEW.category, '')), 'B') ||
          setweight(to_tsvector('simple', coalesce(NEW.author, '')), 'C') ||
          setweight(to_tsvector('simple', coalesce(NEW.description, '')), 'D') ||
          setweight(to_tsvector('simple', coalesce(NEW.introduction, '')), 'D');
        RETURN NEW;
      END;
      $$ LANGUAGE plpgsql
    `)
    await client.query(`
      DROP TRIGGER IF EXISTS trg_videos_tsv ON videos
    `)
    await client.query(`
      CREATE TRIGGER trg_videos_tsv BEFORE INSERT OR UPDATE ON videos
        FOR EACH ROW EXECUTE FUNCTION videos_tsv_update()
    `)

    await client.query(`ALTER TABLE users ADD COLUMN IF NOT EXISTS slug VARCHAR(8) UNIQUE`)
    await client.query(`ALTER TABLE videos ADD COLUMN IF NOT EXISTS slug VARCHAR(8) UNIQUE`)
    await client.query(`ALTER TABLE users ADD COLUMN IF NOT EXISTS display_name VARCHAR(100) DEFAULT ''`)
    await client.query(`ALTER TABLE videos ADD COLUMN IF NOT EXISTS is_deleted BOOLEAN DEFAULT false`)
    await client.query(`ALTER TABLE videos ADD COLUMN IF NOT EXISTS video_url_hls VARCHAR(500) DEFAULT ''`)
    await client.query(`ALTER TABLE videos ADD COLUMN IF NOT EXISTS video_url_360p VARCHAR(500) DEFAULT ''`)
    await client.query(`ALTER TABLE videos ADD COLUMN IF NOT EXISTS video_url_720p VARCHAR(500) DEFAULT ''`)
    await client.query(`ALTER TABLE videos ADD COLUMN IF NOT EXISTS video_url_1080p VARCHAR(500) DEFAULT ''`)
    await client.query(`ALTER TABLE messages ADD COLUMN IF NOT EXISTS message_type VARCHAR(20) DEFAULT 'dm'`)
    await client.query(`ALTER TABLE messages ADD COLUMN IF NOT EXISTS image_url VARCHAR(500) DEFAULT ''`)
    await client.query(`ALTER TABLE comments ADD COLUMN IF NOT EXISTS images TEXT DEFAULT ''`)

    await client.query(`
      CREATE TABLE IF NOT EXISTS forum_categories (
        id SERIAL PRIMARY KEY,
        name VARCHAR(100) NOT NULL,
        description TEXT DEFAULT '',
        slug VARCHAR(50) UNIQUE NOT NULL,
        icon VARCHAR(50) DEFAULT 'fa-comments',
        display_order INTEGER DEFAULT 0,
        topic_count INTEGER DEFAULT 0,
        post_count INTEGER DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      )
    `)
    await client.query(`
      CREATE TABLE IF NOT EXISTS forum_topics (
        id SERIAL PRIMARY KEY,
        title VARCHAR(255) NOT NULL,
        slug VARCHAR(50) UNIQUE NOT NULL,
        category_id INTEGER NOT NULL REFERENCES forum_categories(id) ON DELETE CASCADE,
        author_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        content TEXT DEFAULT '',
        view_count INTEGER DEFAULT 0,
        reply_count INTEGER DEFAULT 0,
        is_pinned BOOLEAN DEFAULT false,
        is_locked BOOLEAN DEFAULT false,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        last_post_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      )
    `)
    await client.query(`
      CREATE TABLE IF NOT EXISTS forum_posts (
        id SERIAL PRIMARY KEY,
        topic_id INTEGER NOT NULL REFERENCES forum_topics(id) ON DELETE CASCADE,
        author_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        content TEXT NOT NULL,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS site_configs (
        key VARCHAR(100) PRIMARY KEY,
        value TEXT DEFAULT '',
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      )
    `)

    const catCount = await client.query('SELECT count(*) FROM forum_categories')
    if (parseInt(catCount.rows[0].count) === 0) {
      await client.query(`
        INSERT INTO forum_categories (name, description, slug, icon, display_order) VALUES
        ('综合讨论', '各类话题的综合性讨论区', 'general', 'fa-comments', 1),
        ('技术交流', '技术经验分享与讨论', 'tech', 'fa-code', 2),
        ('资源分享', '优质资源推荐与分享', 'resources', 'fa-download', 3),
        ('站务反馈', '对网站的建议与问题反馈', 'feedback', 'fa-bullhorn', 4)
      `)
    }

    console.log('PostgreSQL tables initialized')

    const adminExists = await client.query('SELECT id FROM users WHERE username = $1', ['admin'])
    let adminId
    if (!adminExists.rows.length) {
      const adminSlug = await uniqueSlug('users')
      const salt = await bcrypt.genSalt(10)
      const hash = await bcrypt.hash('admin123', salt)
      const newAdmin = await client.query(
        'INSERT INTO users (slug, username, password, role) VALUES ($1, $2, $3, $4) RETURNING id',
        [adminSlug, 'admin', hash, 'admin']
      )
      adminId = newAdmin.rows[0].id
      console.log('Default admin user created (admin / admin123) slug:', adminSlug)
      await client.query('INSERT INTO user_profiles (user_id) VALUES ($1)', [adminId])
    } else {
      adminId = adminExists.rows[0].id
      if (!adminExists.rows[0].slug) {
        const slug = await uniqueSlug('users')
        await client.query('UPDATE users SET slug = $1 WHERE id = $2', [slug, adminId])
      }
      const profileCheck = await client.query('SELECT id FROM user_profiles WHERE user_id = $1', [adminId])
      if (!profileCheck.rows.length) {
        await client.query('INSERT INTO user_profiles (user_id) VALUES ($1)', [adminId])
      }
    }

    // 备用超级管理员账号：账号名「超级管理员」，拥有最高权限（可封禁普通管理员），
    // 且自身不可被封禁，作为主管理员账号的应急备份。
    const superExists = await client.query('SELECT id FROM users WHERE username = $1', ['超级管理员'])
    if (!superExists.rows.length) {
      const superSlug = await uniqueSlug('users')
      const superSalt = await bcrypt.genSalt(10)
      const superHash = await bcrypt.hash('super123456', superSalt)
      const superAdmin = await client.query(
        'INSERT INTO users (slug, username, password, role) VALUES ($1, $2, $3, $4) RETURNING id',
        [superSlug, '超级管理员', superHash, 'super_admin']
      )
      await client.query('INSERT INTO user_profiles (user_id) VALUES ($1)', [superAdmin.rows[0].id])
      console.log('Backup super admin created (超级管理员 / super123456) slug:', superSlug)
    }

    const missingSlugs = await client.query('SELECT id FROM videos WHERE slug IS NULL')
    for (const row of missingSlugs.rows) {
      const slug = await uniqueSlug('videos')
      await client.query('UPDATE videos SET slug = $1 WHERE id = $2', [slug, row.id])
    }

    const carouselCount = await client.query('SELECT count(*) FROM carousels')
    if (parseInt(carouselCount.rows[0].count) === 0) {
      await client.query(`
        INSERT INTO carousels (image_url, link, title, "order") VALUES
        ('/images/banner1.jpg', '/', 'Cocokalo 二次元视频平台', 1),
        ('/images/banner2.jpg', '/', '发现精彩番剧', 2)
      `)
      console.log('Seed carousel data inserted')
    }

    const noticeCount = await client.query('SELECT count(*) FROM notices')
    if (parseInt(noticeCount.rows[0].count) === 0) {
      await client.query(`
        INSERT INTO notices (title, content, "order", is_active) VALUES
        ('欢迎来到 Cocokalo', '这是一个二次元视频平台，欢迎投稿与互动～', 1, true)
      `)
      console.log('Seed notice data inserted')
    }


  } catch (err) {
    console.error('PostgreSQL init error:', err)
  } finally {
    client.release()
  }
}

export async function findUserByUsername(username: string) {
  const result = await queryPg('SELECT * FROM users WHERE username = $1', [username])
  return result.rows[0] || null
}

export async function findUserById(id: number) {
  const result = await queryPg('SELECT id, username, role, email, avatar_url, created_at FROM users WHERE id = $1', [id])
  return result.rows[0] || null
}

export async function findAllUsers() {
  const result = await queryPg('SELECT id, username, role, email, avatar_url, created_at FROM users ORDER BY created_at DESC')
  return result.rows
}

export async function comparePassword(plain: string, hash: string) {
  return bcrypt.compare(plain, hash)
}

export async function getUserProfile(userId: number) {
  const result = await queryPg(
    `SELECT u.*, COALESCE(u.display_name,'') display_name, COALESCE(p.bio,'') bio, COALESCE(p.gender,'') gender,
            p.birthday, COALESCE(p."location",'') "location", COALESCE(p.website,'') website,
            COALESCE(p.followers,0) followers, COALESCE(p."following",0) "following",
            COALESCE(p.likes,0) likes
     FROM users u
     LEFT JOIN user_profiles p ON p.user_id = u.id
     WHERE u.id = $1`, [userId])
  return result.rows[0] || null
}

export async function updateUserProfile(userId: number, data: Record<string, any>) {
  const allowedProfile = ['bio', 'gender', 'location', 'website']
  const allowedUser = ['display_name']
  const profileEntries = Object.entries(data).filter(([k]) => allowedProfile.includes(k))
  const userEntries = Object.entries(data).filter(([k]) => allowedUser.includes(k))
  
  // 更新 user_profiles 表
  if (profileEntries.length) {
    const quote = (k: string) => k === 'location' ? '"location"' : k
    const sets = profileEntries.map(([k], i) => `${quote(k)} = $${i + 1}`)
    const values = profileEntries.map(([, v]) => v || '')
    values.push(userId)
    await queryPg(`UPDATE user_profiles SET ${sets.join(', ')}, updated_at = CURRENT_TIMESTAMP WHERE user_id = $${values.length}`, values)
  }
  
  // 更新 users 表 (display_name)
  if (userEntries.length) {
    const sets = userEntries.map(([k], i) => `${k} = $${i + 1}`)
    const values = userEntries.map(([, v]) => v || '')
    values.push(userId)
    await queryPg(`UPDATE users SET ${sets.join(', ')}, updated_at = CURRENT_TIMESTAMP WHERE id = $${values.length}`, values)
  }
}

export async function getUserStats(userId: number) {
  const [favCount, histCount, videoCount, msgCount, actCount] = await Promise.all([
    queryPg('SELECT count(*) FROM favorites WHERE user_id = $1', [userId]),
    queryPg('SELECT count(*) FROM watch_history WHERE user_id = $1', [userId]),
    queryPg('SELECT count(*) FROM videos WHERE author = (SELECT username FROM users WHERE id = $1)', [userId]),
    queryPg('SELECT count(*) FROM messages WHERE to_user_id = $1 AND is_read = false', [userId]),
    queryPg('SELECT count(*) FROM activities WHERE user_id = $1', [userId]),
  ])
  return {
    favorites: parseInt(favCount.rows[0].count),
    history: parseInt(histCount.rows[0].count),
    videos: parseInt(videoCount.rows[0].count),
    unreadMessages: parseInt(msgCount.rows[0].count),
    activities: parseInt(actCount.rows[0].count),
  }
}

export async function getUserFavorites(userId: number, folderId?: number) {
  let query = `SELECT f.id, f.created_at, v.*
               FROM favorites f
               JOIN videos v ON v.id = f.video_id
               WHERE f.user_id = $1`
  const params: any[] = [userId]
  if (folderId) {
    query += ' AND f.folder_id = $2'
    params.push(folderId)
  }
  query += ' ORDER BY f.created_at DESC'
  const result = await queryPg(query, params)
  return result.rows
}

export async function getFavoriteFolders(userId: number) {
  const result = await queryPg(
    `SELECT ff.*, COALESCE(fc.count,0) video_count
     FROM favorite_folders ff
     LEFT JOIN (SELECT folder_id, count(*) count FROM favorites GROUP BY folder_id) fc ON fc.folder_id = ff.id
     WHERE ff.user_id = $1 ORDER BY ff.created_at DESC`, [userId])
  return result.rows
}

export async function getUserHistory(userId: number, limit = 50) {
  const result = await queryPg(
    `SELECT wh.*, v.title, v.slug, v.image_url, v.video_type, v.video_time, v.author, v.watch_volue, v.like_volue
     FROM watch_history wh
     JOIN videos v ON v.id = wh.video_id
     WHERE wh.user_id = $1
     ORDER BY wh.watched_at DESC LIMIT $2`, [userId, limit])
  return result.rows
}

export async function getUserMessages(userId: number, type?: string) {
  let query = `SELECT m.*,
     u.username from_username, u.display_name from_display_name, u.avatar_url from_avatar,
     u2.username to_username, u2.display_name to_display_name, u2.avatar_url to_avatar
     FROM messages m
     LEFT JOIN users u ON u.id = m.from_user_id
     LEFT JOIN users u2 ON u2.id = m.to_user_id
     WHERE`
  const params: any[] = [userId]

  if (type === 'dm') {
    params.push(type)
    query += ` (m.to_user_id = $1 OR m.from_user_id = $1) AND m.message_type = $${params.length}`
  } else {
    query += ` m.to_user_id = $1`
    if (type) {
      params.push(type)
      query += ` AND m.message_type = $${params.length}`
    }
  }
  query += ` ORDER BY m.created_at DESC LIMIT 100`
  const result = await queryPg(query, params)
  return result.rows
}

export async function getActivities(userId?: number, limit = 20) {
  if (userId) {
    const result = await queryPg(
      `SELECT a.*, u.username, u.display_name, u.avatar_url
       FROM activities a JOIN users u ON u.id = a.user_id
       WHERE a.user_id = $1
       ORDER BY a.created_at DESC LIMIT $2`, [userId, limit])
    return result.rows
  }
  const result = await queryPg(
    `SELECT a.*, u.username, u.display_name, u.avatar_url
     FROM activities a JOIN users u ON u.id = a.user_id
     ORDER BY a.created_at DESC LIMIT $1`, [limit])
  return result.rows
}

export function getAuthFromEvent(event: any) {
  try {
    console.log('[getAuthFromEvent] cookies:', event.node?.req?.headers?.cookie)
    const tokenCookie = getCookie(event, 'cocokalo_token')
    console.log('[getAuthFromEvent] tokenCookie:', tokenCookie)
    if (tokenCookie) {
      const decoded = verifyToken(tokenCookie)
      console.log('[getAuthFromEvent] decoded from token:', decoded)
      if (decoded) return decoded
    }
    const userCookie = getCookie(event, 'cocokalo_user')
    console.log('[getAuthFromEvent] userCookie:', userCookie)
    if (userCookie) {
      const parsed = JSON.parse(userCookie)
      if (parsed.token) {
        const decoded = verifyToken(parsed.token)
        console.log('[getAuthFromEvent] decoded from userCookie token:', decoded)
        if (decoded) return decoded
      }
      console.log('[getAuthFromEvent] returning from userCookie:', { userId: parsed.userId, username: parsed.username, role: parsed.role })
      return { userId: parsed.userId, username: parsed.username, role: parsed.role }
    }
  } catch (e) {
    console.error('[getAuthFromEvent] error:', e)
  }
  console.log('[getAuthFromEvent] returning null')
  return null
}

export async function requireAuth(event: any) {
  const auth = getAuthFromEvent(event)
  if (!auth) throw createError({ statusCode: 401, message: '请先登录' })

  // 管理员（含超级管理员）不受账号封禁限制：封禁是面向普通用户的处罚，
  // 且管理员可在后台自行解封，锁死管理员会导致无人能管理。
  if (auth.role === 'admin' || auth.role === 'super_admin') {
    return auth
  }

  console.log('[requireAuth] auth:', auth)
  const { getActiveBan } = await import('./pg')
  const ban = await getActiveBan(parseInt(auth.userId))
  console.log('[requireAuth] ban check result:', ban)
  if (ban) {
    const message = ban.expires_at
      ? `账号被封禁至 ${new Date(ban.expires_at).toLocaleString()}，原因：${ban.reason}`
      : `账号被永久封禁，原因：${ban.reason}`
    throw createError({ statusCode: 403, message, data: { ban } })
  }

  console.log('[requireAuth] returning auth:', auth)
  return auth
}

export async function requireAdmin(event: any) {
  const auth = await requireAuth(event)
  if (auth.role !== 'admin' && auth.role !== 'super_admin') throw createError({ statusCode: 401, message: '需要管理员权限' })
  return auth
}

export async function getCreatorStats(userId: number) {
  const result = await queryPg(
    `SELECT
       COALESCE(SUM(CASE WHEN v.watch_volue ~ '^\\d+$' THEN v.watch_volue::INTEGER ELSE 0 END),0) total_views,
       COALESCE(SUM(CASE WHEN v.like_volue ~ '^\\d+$' THEN v.like_volue::INTEGER ELSE 0 END),0) total_likes,
       count(*) total_videos
     FROM videos v
     WHERE v.author = (SELECT username FROM users WHERE id = $1)`, [userId])
  return result.rows[0] || { total_views: 0, total_likes: 0, total_videos: 0 }
}

export async function getComments(videoSlug: string) {
  const result = await queryPg(
    `SELECT c.*, u.avatar_url as user_avatar, u.display_name as user_display_name
     FROM comments c
     LEFT JOIN users u ON u.id = c.user_id
     WHERE c.video_slug = $1
     ORDER BY c.parent_id NULLS FIRST, c.created_at ASC`, [videoSlug])
  return result.rows
}

export async function addComment(videoSlug: string, userId: number, username: string, avatarUrl: string | null, content: string, parentId?: number, images?: string[]) {
  const result = await queryPg(
    `INSERT INTO comments (video_slug, user_id, username, avatar_url, content, parent_id, images)
     VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING *`,
    [videoSlug, userId, username, avatarUrl, content, parentId || null, images ? JSON.stringify(images) : ''])
  return result.rows[0]
}

export async function getDanmaku(videoSlug: string) {
  const result = await queryPg(
    'SELECT * FROM danmaku WHERE video_slug = $1 ORDER BY time ASC', [videoSlug])
  return result.rows
}

export async function addDanmaku(videoSlug: string, userId: number | null, username: string, content: string, time: number, type: string, color: string) {
  const result = await queryPg(
    `INSERT INTO danmaku (video_slug, user_id, username, content, time, type, color)
     VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING *`,
    [videoSlug, userId, username, content, time, type, color])
  return result.rows[0]
}

export async function createBan(userId: number, operatorId: number, type: string, reason: string, durationMinutes?: number) {
  const expiresAt = durationMinutes
    ? new Date(Date.now() + durationMinutes * 60 * 1000).toISOString()
    : null
  const result = await queryPg(
    `INSERT INTO user_bans (user_id, operator_id, type, reason, duration, expires_at)
     VALUES ($1, $2, $3, $4, $5, $6) RETURNING *`,
    [userId, operatorId, type, reason, durationMinutes || null, expiresAt])
  return result.rows[0]
}

export async function getActiveBan(userId: number) {
  const result = await queryPg(
    `SELECT ub.*, u.username as operator_name
     FROM user_bans ub
     LEFT JOIN users u ON u.id = ub.operator_id
     WHERE ub.user_id = $1 AND ub.is_active = true
       AND (ub.expires_at IS NULL OR ub.expires_at > NOW())
     ORDER BY ub.created_at DESC LIMIT 1`, [userId])
  return result.rows[0] || null
}

export async function getBanList(page = 1, size = 20, status?: string) {
  const offset = (page - 1) * size
  let where = ''
  const params: any[] = []
  if (status === 'active') {
    where = 'WHERE ub.is_active = true AND (ub.expires_at IS NULL OR ub.expires_at > NOW())'
  } else if (status === 'expired') {
    where = 'WHERE ub.is_active = false OR (ub.expires_at IS NOT NULL AND ub.expires_at <= NOW())'
  }
  const countResult = await queryPg(`SELECT count(*) FROM user_bans ub ${where}`, params)
  const total = parseInt(countResult.rows[0].count)
  params.push(size, offset)
  const result = await queryPg(
    `SELECT ub.*, u.username as target_username, u.avatar_url as target_avatar,
            o.username as operator_name
     FROM user_bans ub
     JOIN users u ON u.id = ub.user_id
     LEFT JOIN users o ON o.id = ub.operator_id
     ${where}
     ORDER BY ub.created_at DESC
     LIMIT $${params.length - 1} OFFSET $${params.length}`, params)
  return { total, bans: result.rows }
}

export async function updateBan(banId: number, updates: { is_active?: boolean; reason?: string; duration?: number }) {
  const set: string[] = []
  const values: any[] = []
  if (updates.is_active !== undefined) { values.push(updates.is_active); set.push(`is_active = $${values.length}`) }
  if (updates.reason !== undefined) { values.push(updates.reason); set.push(`reason = $${values.length}`) }
  if (updates.duration !== undefined) {
    values.push(updates.duration)
    const expiresAt = updates.duration
      ? new Date(Date.now() + updates.duration * 60 * 1000).toISOString()
      : null
    values.push(expiresAt)
    set.push(`duration = $${values.length - 1}, expires_at = $${values.length}`)
  }
  if (!set.length) return null
  set.push(`updated_at = CURRENT_TIMESTAMP`)
  values.push(banId)
  const result = await queryPg(
    `UPDATE user_bans SET ${set.join(', ')} WHERE id = $${values.length} RETURNING *`, values)
  return result.rows[0] || null
}

export async function deleteBan(banId: number) {
  await queryPg('DELETE FROM user_bans WHERE id = $1', [banId])
  return { success: true }
}

export async function getBanById(banId: number) {
  const result = await queryPg(
    `SELECT ub.*, u.username as target_username, u.avatar_url as target_avatar,
            o.username as operator_name
     FROM user_bans ub
     JOIN users u ON u.id = ub.user_id
     LEFT JOIN users o ON o.id = ub.operator_id
     WHERE ub.id = $1`, [banId])
  return result.rows[0] || null
}
