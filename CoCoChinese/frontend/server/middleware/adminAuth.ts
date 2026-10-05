import { getCookie } from 'h3'
import { verifyToken } from '~/server/utils/jwt'

export default defineEventHandler(async (event) => {
  const path = event.path
  if (path.startsWith('/admin') && path !== '/admin/login') {
    const token = getCookie(event, 'cocokalo_token')
    if (!token) {
      return sendRedirect(event, '/admin/login')
    }
    try {
      const decoded = verifyToken(token)
      if (!decoded || decoded.role !== 'admin') {
        return sendRedirect(event, '/admin/login')
      }
    } catch {
      return sendRedirect(event, '/admin/login')
    }
  }
})