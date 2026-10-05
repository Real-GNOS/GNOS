import { signToken } from '../../../utils/jwt'

export default defineEventHandler(async (event) => {
  const provider = getRouterParam(event, 'provider')
  const { code, state } = getQuery(event)

  const savedState = getCookie(event, 'oauth_state')
  if (state !== savedState) {
    throw createError({ statusCode: 400, message: '状态验证失败' })
  }

  const siteUrl = process.env.SITE_URL || 'http://localhost:3000'
  const clientId = process.env[`OAUTH_${provider!.toUpperCase()}_CLIENT_ID`]
  const clientSecret = process.env[`OAUTH_${provider!.toUpperCase()}_CLIENT_SECRET`]
  const redirectUri = `${siteUrl}/api/oauth/${provider}/callback`

  try {
    let tokenUrl = ''
    let userUrl = ''
    let emailUrl = ''
    let tokenBody: Record<string, string> = {}

    switch (provider) {
      case 'google':
        tokenUrl = 'https://oauth2.googleapis.com/token'
        userUrl = 'https://www.googleapis.com/oauth2/v2/userinfo'
        tokenBody = { code: code as string, client_id: clientId!, client_secret: clientSecret!, redirect_uri: redirectUri, grant_type: 'authorization_code' }
        break
      case 'discord':
        tokenUrl = 'https://discord.com/api/oauth2/token'
        userUrl = 'https://discord.com/api/users/@me'
        tokenBody = { code: code as string, client_id: clientId!, client_secret: clientSecret!, redirect_uri: redirectUri, grant_type: 'authorization_code' }
        break
      case 'github':
        tokenUrl = 'https://github.com/login/oauth/access_token'
        userUrl = 'https://api.github.com/user'
        emailUrl = 'https://api.github.com/user/emails'
        tokenBody = { code: code as string, client_id: clientId!, client_secret: clientSecret!, redirect_uri: redirectUri }
        break
    }

    const tokenRes = await $fetch(tokenUrl, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: tokenBody,
    })

    const accessToken = (tokenRes as any).access_token
    if (!accessToken) throw new Error('Failed to get access token')

    const userRes = await $fetch(userUrl, {
      headers: { Authorization: `Bearer ${accessToken}` },
    })

    const profile: any = userRes
    let oauthId = ''
    let email = ''
    let avatarUrl = ''
    let username = ''

    switch (provider) {
      case 'google':
        oauthId = profile.id
        email = profile.email
        avatarUrl = profile.picture
        username = profile.name
        break
      case 'discord':
        oauthId = profile.id
        email = profile.email || ''
        avatarUrl = profile.avatar ? `https://cdn.discordapp.com/avatars/${profile.id}/${profile.avatar}.png` : ''
        username = profile.username
        break
      case 'github':
        oauthId = String(profile.id)
        email = profile.email || ''
        avatarUrl = profile.avatar_url
        username = profile.login
        if (!email && emailUrl) {
          const emailsRes: any = await $fetch(emailUrl, { headers: { Authorization: `Bearer ${accessToken}` } })
          const primary = emailsRes.find((e: any) => e.primary)
          email = primary?.email || ''
        }
        break
    }

    if (!email) {
      throw createError({ statusCode: 400, message: '无法获取邮箱信息' })
    }

    const { queryPg } = await import('../../../utils/pg')
    const existingAccount = await queryPg(
      'SELECT user_id FROM oauth_accounts WHERE provider = $1 AND provider_id = $2',
      [provider, oauthId]
    )

    let userId: number
    if (existingAccount.rows.length) {
      userId = existingAccount.rows[0].user_id
    } else {
      const existingUser = await queryPg('SELECT id FROM users WHERE email = $1', [email])
      if (existingUser.rows.length) {
        userId = existingUser.rows[0].id
        await queryPg(
          'INSERT INTO oauth_accounts (user_id, provider, provider_id, email, avatar_url) VALUES ($1, $2, $3, $4, $5)',
          [userId, provider, oauthId, email, avatarUrl]
        )
      } else {
        const { uniqueSlug } = await import('../../../utils/pg')
        const bcrypt = await import('bcryptjs')
        const slug = await uniqueSlug('users')
        const tempPassword = require('crypto').randomBytes(16).toString('hex')
        const hash = await bcrypt.hash(tempPassword, 10)
        const newUser = await queryPg(
          'INSERT INTO users (slug, username, password, email, avatar_url, role) VALUES ($1, $2, $3, $4, $5, $6) RETURNING id',
          [slug, username, hash, email, avatarUrl, 'user']
        )
        userId = newUser.rows[0].id
        await queryPg('INSERT INTO user_profiles (user_id) VALUES ($1)', [userId])
        await queryPg(
          'INSERT INTO oauth_accounts (user_id, provider, provider_id, email, avatar_url) VALUES ($1, $2, $3, $4, $5)',
          [userId, provider, oauthId, email, avatarUrl]
        )
      }
    }

    const token = signToken({ userId, username: profile.name || profile.login, role: 'user' })
    setCookie(event, 'cocokalo_token', token, { maxAge: 604800, path: '/' })
    setCookie(event, 'cocokalo_user', JSON.stringify({ userId, username: profile.name || profile.login, role: 'user', token }), { maxAge: 604800, path: '/' })

    return sendRedirect(event, '/')
  } catch (e: any) {
    throw createError({ statusCode: 500, message: `OAuth 登录失败: ${e.message}` })
  }
})
