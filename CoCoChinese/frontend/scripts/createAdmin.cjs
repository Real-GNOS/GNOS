const pg = require('pg')
const bcrypt = require('bcryptjs')
const crypto = require('crypto')

const DB_NAME = process.env.PG_DB || 'cocokalo'

function makePool(dbName) {
  return new pg.Pool({
    host: process.env.PG_HOST || 'localhost',
    port: parseInt(process.env.PG_PORT || '5432'),
    database: dbName,
    user: process.env.PG_USER || 'postgres',
    password: process.env.PG_PASSWORD || '123',
    max: 10,
    idleTimeoutMillis: 30000,
  })
}

function generateSlug(length = 8) {
  const chars = 'abcdefghijklmnopqrstuvwxyz0123456789'
  let slug = ''
  const bytes = crypto.randomBytes(length)
  for (let i = 0; i < length; i++) {
    slug += chars[bytes[i] % chars.length]
  }
  return slug
}

async function createAdminUser(username, password) {
  const pool = makePool('postgres')
  const client = await pool.connect()
  try {
    const res = await client.query(`SELECT 1 FROM pg_database WHERE datname = $1`, [DB_NAME])
    if (!res.rows.length) {
      await client.query(`CREATE DATABASE "${DB_NAME}"`)
      console.log(`Database "${DB_NAME}" created`)
    }
  } finally {
    client.release()
    await pool.end()
  }

  const dbPool = makePool(DB_NAME)
  const dbClient = await dbPool.connect()
  try {
    await dbClient.query(`
      CREATE TABLE IF NOT EXISTS users (
        id SERIAL PRIMARY KEY,
        slug VARCHAR(8) UNIQUE,
        username VARCHAR(50) UNIQUE NOT NULL,
        password VARCHAR(255) NOT NULL,
        email VARCHAR(255) UNIQUE,
        role VARCHAR(20) DEFAULT 'user',
        avatar_url VARCHAR(500),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
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
    `)

    const existing = await dbClient.query('SELECT id FROM users WHERE username = $1', [username])
    if (existing.rows.length) {
      await dbClient.query("UPDATE users SET role = 'admin' WHERE username = $1", [username])
      console.log(`User "${username}" updated to admin role`)
    } else {
      const adminSlug = generateSlug()
      const salt = await bcrypt.genSalt(10)
      const hash = await bcrypt.hash(password, salt)
      const newAdmin = await dbClient.query(
        'INSERT INTO users (slug, username, password, role) VALUES ($1, $2, $3, $4) RETURNING id',
        [adminSlug, username, hash, 'admin']
      )
      const adminId = newAdmin.rows[0].id
      await dbClient.query('INSERT INTO user_profiles (user_id) VALUES ($1)', [adminId])
      console.log(`Admin user created: ${username} / ${password} (slug: ${adminSlug})`)
    }
  } finally {
    dbClient.release()
    await dbPool.end()
  }
}

const username = process.argv[2] || 'admin'
const password = process.argv[3] || 'admin123'

createAdminUser(username, password).catch(err => {
  console.error('Error:', err)
  process.exit(1)
})