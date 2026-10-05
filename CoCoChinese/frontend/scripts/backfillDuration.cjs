const { Pool } = require('pg')
const path = require('path')
const fs = require('fs')
const { execSync } = require('child_process')

const pool = new Pool({
  host: process.env.PG_HOST || 'localhost',
  port: parseInt(process.env.PG_PORT || '5432'),
  database: process.env.PG_DB || 'cocokalo',
  user: process.env.PG_USER || 'postgres',
  password: process.env.PG_PASSWORD || '123',
})

async function getDuration(filePath) {
  try {
    const resolved = path.resolve(__dirname, '..', 'public', filePath.replace(/^\//, ''))
    if (!fs.existsSync(resolved)) return null
    const out = execSync(
      `ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "${resolved}"`,
      { encoding: 'utf8', timeout: 10000 }
    ).trim()
    const totalSec = Math.round(parseFloat(out))
    if (isNaN(totalSec) || totalSec <= 0) return null
    const h = Math.floor(totalSec / 3600)
    const m = Math.floor((totalSec % 3600) / 60)
    const s = totalSec % 60
    if (h > 0) {
      return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
    }
    return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  } catch {
    return null
  }
}

async function main() {
  const client = await pool.connect()
  try {
    const result = await client.query(
      "SELECT id, slug, video_url, video_time FROM videos WHERE video_time IS NULL OR video_time = '' OR video_time = '00:00'"
    )
    console.log(`Found ${result.rows.length} videos without duration`)
    let updated = 0
    for (const row of result.rows) {
      if (!row.video_url) continue
      const duration = await getDuration(row.video_url)
      if (duration) {
        await client.query('UPDATE videos SET video_time = $1 WHERE id = $2', [duration, row.id])
        console.log(`  [${row.slug}] ${row.video_url} -> ${duration}`)
        updated++
      } else {
        console.log(`  [${row.slug}] ${row.video_url} -> SKIP (not found or 0s)`)
      }
    }
    console.log(`Done. Updated ${updated} videos.`)
  } finally {
    client.release()
    await pool.end()
  }
}

main().catch(console.error)
