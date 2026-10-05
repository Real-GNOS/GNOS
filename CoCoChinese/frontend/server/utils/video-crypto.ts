import crypto from 'crypto'

const sessions = new Map<string, { key: Buffer, expiresAt: number }>()

const SESSION_TTL = 3600000

export function createSession(): string {
  const sessionId = crypto.randomUUID()
  const key = crypto.randomBytes(32)
  sessions.set(sessionId, { key, expiresAt: Date.now() + SESSION_TTL })
  return sessionId
}

export function getSessionKey(sessionId: string): Buffer | null {
  const session = sessions.get(sessionId)
  if (!session || session.expiresAt < Date.now()) {
    sessions.delete(sessionId)
    return null
  }
  return session.key
}

export function aesEncrypt(key: Buffer, plaintext: string): string {
  const iv = crypto.randomBytes(12)
  const cipher = crypto.createCipheriv('aes-256-gcm', key, iv)
  let encrypted = cipher.update(plaintext, 'utf8', 'hex')
  encrypted += cipher.final('hex')
  const tag = cipher.getAuthTag().toString('hex')
  return iv.toString('hex') + ':' + tag + ':' + encrypted
}

export function aesDecrypt(key: Buffer, encrypted: string): string | null {
  try {
    const [ivHex, tagHex, dataHex] = encrypted.split(':')
    const iv = Buffer.from(ivHex, 'hex')
    const tag = Buffer.from(tagHex, 'hex')
    const decipher = crypto.createDecipheriv('aes-256-gcm', key, iv)
    decipher.setAuthTag(tag)
    let decrypted = decipher.update(dataHex, 'hex', 'utf8')
    decrypted += decipher.final('utf8')
    return decrypted
  } catch {
    return null
  }
}

const URL_FIELDS = ['video_url', 'video_url_360p', 'video_url_720p', 'video_url_1080p', 'video_url_hls', 'videoUrl', 'videoUrlHls', 'videoUrl360p', 'videoUrl720p', 'videoUrl1080p']

export function encryptResponseUrls(data: any, sessionId?: string): any {
  if (!sessionId) return data
  const key = getSessionKey(sessionId)
  if (!key) return data
  const result = { ...data }
  let didEncrypt = false
  for (const field of URL_FIELDS) {
    if (result[field]) {
      result[field] = aesEncrypt(key, result[field])
      didEncrypt = true
    }
  }
  if (didEncrypt) result._enc = true
  return result
}

setInterval(() => {
  const now = Date.now()
  for (const [id, session] of sessions) {
    if (session.expiresAt < now) sessions.delete(id)
  }
}, 60000)
