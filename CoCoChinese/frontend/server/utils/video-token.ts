import jwt from 'jsonwebtoken'

const JWT_SECRET = process.env.VIDEO_TOKEN_SECRET || process.env.JWT_SECRET || 'cocokalo-jwt-secret-2026'

export interface VideoTokenPayload {
  videoId: string
  userId?: number
  canDownload?: boolean
  exp: number
}

export function generateVideoToken(videoId: string, userId?: number, canDownload = false) {
  return jwt.sign(
    { videoId, userId, canDownload },
    JWT_SECRET,
    { expiresIn: '30m' }
  )
}

export function verifyVideoToken(token: string): VideoTokenPayload | null {
  try {
    return jwt.verify(token, JWT_SECRET) as VideoTokenPayload
  } catch {
    return null
  }
}
