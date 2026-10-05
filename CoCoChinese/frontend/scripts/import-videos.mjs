import pg from 'pg'
import { randomBytes } from 'crypto'
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'fs'
import { execSync } from 'child_process'
import path from 'path'

const pool = new pg.Pool({
  host: process.env.PG_HOST || 'localhost',
  port: parseInt(process.env.PG_PORT || '5432'),
  database: process.env.PG_DB || 'cocokalo',
  user: process.env.PG_USER || 'postgres',
  password: String(process.env.PG_PASSWORD || '123'),
})

function generateSlug(length = 8) {
  const chars = 'abcdefghijklmnopqrstuvwxyz0123456789'
  let slug = ''
  const bytes = randomBytes(length)
  for (let i = 0; i < length; i++) {
    slug += chars[bytes[i] % chars.length]
  }
  return slug
}

async function uniqueSlug() {
  for (let i = 0; i < 20; i++) {
    const slug = generateSlug(8)
    const res = await pool.query(`SELECT 1 FROM videos WHERE slug = $1`, [slug])
    if (!res.rows.length) return slug
  }
  throw new Error('Failed to generate unique slug')
}

function getVideoDuration(filePath) {
  const output = execSync(
    `ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "${filePath}"`,
    { encoding: 'utf-8', timeout: 30000 }
  )
  const totalSec = Math.round(parseFloat(output.trim()))
  if (!totalSec || totalSec <= 0) return '00:00'
  const h = Math.floor(totalSec / 3600)
  const m = Math.floor((totalSec % 3600) / 60)
  const s = totalSec % 60
  if (h > 0) return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
}

const sourceDir = '/home/elaina/myblog/博客系统/uploads'
const storageDir = '/home/elaina/Cocokalo/frontend/storage/videos'
const coverImageUrl = '/images/w24hxh6x_cover.jpg'

mkdirSync(storageDir, { recursive: true })

const files = [
  '1339cc99929b49dabdf33e2ca9480e2f.mp4',
  '18502e1863ff4d1db15bd68d6112965f.mp4',
  '1db67073fe9942cb883ebf7ab205ba77.mp4',
  '21f6534d10ee454b91bbab70f7694588.mp4',
  '3f185c1d3b084584b83199a3e7e9a804.mp4',
  '4a54d3b5-cf5a-47b0-b42e-9f7733eddd91.mp4',
  '5b3f52e4b5224b1b892aed72866d9b58.mp4',
  '5d5998ec70a3478e92a0d819735087a3.mp4',
  '79847f6ccd4a4935856af99702d10ca1.mp4',
  '7da888be85bc41918545cf588f0f86e6.mp4',
  '815b706ef5bf43c08c006c59f7f97a33.mp4',
  'd336b6855f314b75b666073d8eadb4d8.mp4',
  'f60046af80e449f0aa69544a27cd1a96.mp4',
  'f79de7582cea44ec8223dc7bc1830aed.mp4',
  'fb92f017eb8b4f10877337ea2341cbaa.mp4',
]

let index = 1
for (const file of files) {
  const sourcePath = path.join(sourceDir, file)
  const slug = await uniqueSlug()
  const destPath = path.join(storageDir, `${slug}.mp4`)

  console.log(`[${index}/${files.length}] Processing: ${file} -> ${slug}.mp4`)

  // Copy the source MP4 directly
  copyFileSync(sourcePath, destPath)

  // Get video duration
  const videoTime = getVideoDuration(destPath)

  const title = `VO大自然${index}`
  const description = 'vo大自然'

  // Insert DB record
  await pool.query(
    `INSERT INTO videos (slug, title, author, author_img, introduction, video_type, video_url, video_url_hls, image_url, tags, video_time, "order")
     VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, 0)`,
    [slug, title, 'admin', '/images/authorImg.webp', description, '', `/videos/${slug}.mp4`, '', coverImageUrl, [], videoTime]
  )

  console.log(`   -> Imported as "${title}" (${videoTime})`)
  index++
}

console.log('\nAll done! Imported', files.length, 'videos.')
await pool.end()
