let sessionId: string | null = null
let cryptoKey: CryptoKey | null = null

const URL_FIELDS = [
  'video_url', 'video_url_360p', 'video_url_720p', 'video_url_1080p', 'video_url_hls',
  'videoUrl', 'videoUrlHls', 'videoUrl360p', 'videoUrl720p', 'videoUrl1080p',
]

function hexToBytes(hex: string): Uint8Array {
  const bytes = new Uint8Array(hex.length / 2)
  for (let i = 0; i < hex.length; i += 2) {
    bytes[i / 2] = parseInt(hex.substring(i, i + 2), 16)
  }
  return bytes
}

async function importKey(keyHex: string): Promise<CryptoKey> {
  const keyBytes = hexToBytes(keyHex)
  return crypto.subtle.importKey('raw', keyBytes, { name: 'AES-GCM' }, false, ['decrypt'])
}

async function aesDecrypt(encrypted: string): Promise<string> {
  if (!cryptoKey) throw new Error('No session key')
  const parts = encrypted.split(':')
  if (parts.length !== 3) throw new Error('Invalid encrypted format')
  const [ivHex, tagHex, dataHex] = parts

  const iv = hexToBytes(ivHex)
  const tag = hexToBytes(tagHex)
  const data = hexToBytes(dataHex)

  const ciphertext = new Uint8Array(data.length + tag.length)
  ciphertext.set(data)
  ciphertext.set(tag, data.length)

  const decrypted = await crypto.subtle.decrypt(
    { name: 'AES-GCM', iv },
    cryptoKey,
    ciphertext
  )
  return new TextDecoder().decode(decrypted)
}

function isEncrypted(val: any): boolean {
  return typeof val === 'string' && val.includes(':') && val.split(':').length === 3
}

async function decryptItem(item: Record<string, any>): Promise<Record<string, any>> {
  const result = { ...item }
  for (const field of URL_FIELDS) {
    if (result[field] && isEncrypted(result[field])) {
      try {
        result[field] = await aesDecrypt(result[field])
      } catch {
        delete result[field]
      }
    }
  }
  delete result._enc
  return result
}

export function useVideoCrypto() {
  async function negotiateSession(): Promise<string | null> {
    try {
      const res = await $fetch('/api/video/session', { method: 'POST' })
      if (res?.success && res.sessionId) {
        sessionId = res.sessionId
        if (res.key) await setSessionKey(res.key)
        return sessionId
      }
    } catch {}
    return null
  }

  async function setSessionKey(keyHex: string) {
    cryptoKey = await importKey(keyHex)
  }

  function getSessionId(): string | null {
    return sessionId
  }

  async function decryptUrls(data: any): Promise<any> {
    if (!cryptoKey) return data

    if (Array.isArray(data)) {
      const results: any[] = []
      for (const item of data) {
        results.push(item._enc ? await decryptItem(item) : item)
      }
      return results
    }

    if (data?._enc) {
      return decryptItem(data)
    }

    if (data?.rows && Array.isArray(data.rows)) {
      const rows: any[] = []
      for (const item of data.rows) {
        rows.push(item._enc ? await decryptItem(item) : item)
      }
      return { ...data, rows }
    }

    return data
  }

  return { negotiateSession, setSessionKey, getSessionId, decryptUrls }
}
