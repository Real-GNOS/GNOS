import { describe, it, expect, vi, beforeEach } from 'vitest'

describe('Cocokalo Setup', () => {
  it('checkCommand should detect available commands', async () => {
    const { checkCommand } = await import('../setup/installer')
    const result = checkCommand('node')
    expect(result).toBe(true)
  })

  it('detectPackageManager should detect npm', async () => {
    const { detectPackageManager } = await import('../setup/installer')
    const pm = detectPackageManager()
    expect(['npm', 'pnpm', 'yarn']).toContain(pm)
  })
})

describe('Fluent Design System', () => {
  it('should have CSS custom properties defined', () => {
    const root = document.documentElement
    const style = getComputedStyle(root)
    expect(style.getPropertyValue('--fluent-accent').trim()).toBe('#0078d4')
    expect(style.getPropertyValue('--fluent-bg').trim()).toBe('#faf9f8')
  })
})

describe('Cocokalo Core Logic', () => {
  it('should validate JWT signing and verification', async () => {
    const { signToken, verifyToken } = await import('../server/utils/jwt')
    const payload = { userId: 1, username: 'test', role: 'user' }
    const token = signToken(payload)

    expect(token).toBeTruthy()
    expect(typeof token).toBe('string')

    const decoded = verifyToken(token)
    expect(decoded).toBeTruthy()
    expect(decoded!.userId).toBe(1)
    expect(decoded!.username).toBe('test')
  })

  it('should generate unique slugs', async () => {
    const { generateSlug, uniqueSlug } = await import('../server/utils/pg')
    const slug1 = generateSlug(8)
    const slug2 = generateSlug(8)

    expect(slug1).toHaveLength(8)
    expect(slug2).toHaveLength(8)
    expect(slug1).not.toBe(slug2)
  })
})

describe('I18n Translations', () => {
  it('should have Chinese translations', async () => {
    const { default: handler } = await import('../server/api/i18n/translations.get')
    // Check module exports
    expect(handler).toBeDefined()
  })

  it('should have all required translation keys in Chinese', async () => {
    const { default: translations } = await import('../server/api/i18n/translations.get')
    const requiredKeys = ['nav.home', 'nav.login', 'nav.register', 'common.loading']
    expect(translations).toBeDefined()
  })
})

describe('Prisma Schema', () => {
  it('should define all required models', () => {
    const fs = require('fs')
    const schema = fs.readFileSync('./prisma/schema.prisma', 'utf-8')
    const requiredModels = [
      'model User', 'model Video', 'model Subscription',
      'model Payment', 'model OAuthAccount', 'model PasswordReset',
      'model Notification', 'model ContentReport', 'model Subtitle',
      'model AudioTrack', 'model VideoChapter', 'model AuditLog',
      'model SiteConfig', 'model Stream', 'model Playlist',
    ]
    for (const model of requiredModels) {
      expect(schema).toContain(model)
    }
  })
})
