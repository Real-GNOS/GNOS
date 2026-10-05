import { signToken } from '../../utils/jwt'

export default defineEventHandler(async (event) => {
  const provider = getRouterParam(event, 'provider')
  if (!['google', 'discord', 'github'].includes(provider || '')) {
    throw createError({ statusCode: 400, message: '不支持的 OAuth 提供商' })
  }

  const siteUrl = process.env.SITE_URL || 'http://localhost:3000'
  const clientId = process.env[`OAUTH_${provider!.toUpperCase()}_CLIENT_ID`]
  if (!clientId) {
    throw createError({ statusCode: 500, message: `${provider} OAuth 未配置` })
  }

  const redirectUri = `${siteUrl}/api/oauth/${provider}/callback`
  const state = require('crypto').randomBytes(16).toString('hex')

  let authUrl = ''
  const scopes = {
    google: 'openid email profile',
    discord: 'identify email',
    github: 'read:user user:email',
  }

  switch (provider) {
    case 'google':
      authUrl = `https://accounts.google.com/o/oauth2/v2/auth?client_id=${clientId}&redirect_uri=${redirectUri}&response_type=code&scope=${encodeURIComponent(scopes.google)}&state=${state}&access_type=offline`
      break
    case 'discord':
      authUrl = `https://discord.com/api/oauth2/authorize?client_id=${clientId}&redirect_uri=${redirectUri}&response_type=code&scope=${encodeURIComponent(scopes.discord)}&state=${state}`
      break
    case 'github':
      authUrl = `https://github.com/login/oauth/authorize?client_id=${clientId}&redirect_uri=${redirectUri}&state=${state}&scope=${scopes.github}`
      break
  }

  setCookie(event, 'oauth_state', state, { maxAge: 600, path: '/' })
  return sendRedirect(event, authUrl)
})
