import jwt from 'jsonwebtoken'

const SECRET = process.env.JWT_SECRET || 'cocokalo-jwt-secret-2026'

export function signToken(payload: Record<string, any>) {
  return jwt.sign(payload, SECRET, { expiresIn: '7d' })
}

export function verifyToken(token: string) {
  try {
    return jwt.verify(token, SECRET) as Record<string, any>
  } catch {
    return null
  }
}
