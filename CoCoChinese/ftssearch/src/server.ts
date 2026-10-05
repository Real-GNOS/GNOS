import { createServer, IncomingMessage, ServerResponse } from 'http'
import type { ColumnMapping, ServerConfig, SearchResult } from './types.js'
import type { Ftssearch } from './index.js'

function json(res: ServerResponse, status: number, data: unknown): void {
  res.writeHead(status, { 'Content-Type': 'application/json' })
  res.end(JSON.stringify(data))
}

function parseBody(req: IncomingMessage): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    let body = ''
    req.on('data', (chunk: Buffer) => { body += chunk.toString() })
    req.on('end', () => {
      try {
        resolve(body ? JSON.parse(body) : {})
      } catch {
        reject(new Error('Invalid JSON'))
      }
    })
    req.on('error', reject)
  })
}

export async function startServer(
  fts: Ftssearch,
  config: ServerConfig = {},
): Promise<{ close: () => void }> {
  const port = config.port ?? 3001
  const host = config.host ?? '0.0.0.0'
  const useCors = config.cors ?? true
  const table = config.table ?? 'videos'
  const columns: ColumnMapping[] = config.columns ?? []
  const language = config.language ?? 'simple'

  const server = createServer(async (req, res) => {
    if (useCors) {
      res.setHeader('Access-Control-Allow-Origin', '*')
      res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS')
      res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization')
    }
    if (req.method === 'OPTIONS') {
      res.writeHead(204)
      res.end()
      return
    }

    try {
      const url = new URL(req.url ?? '/', `http://${req.headers.host ?? 'localhost'}`)
      const path = url.pathname
      const method = req.method ?? 'GET'

      if (path === '/search' && method === 'GET') {
        const q = url.searchParams.get('q') ?? ''
        if (!q.trim()) {
          json(res, 400, { success: false, message: 'Missing query parameter "q"' })
          return
        }
        const limit = Math.min(100, Math.max(1, parseInt(url.searchParams.get('limit') ?? '20', 10) || 20))
        const offset = Math.max(0, parseInt(url.searchParams.get('offset') ?? '0', 10) || 0)

        const result = await fts.search(table, columns, q, language, { limit, offset })
        json(res, 200, { success: true, query: q, total: result.total, hits: result.hits })
        return
      }

      if (path === '/stats' && method === 'GET') {
        const cacheStats = fts.getCacheStats()
        json(res, 200, { success: true, cache: cacheStats })
        return
      }

      if (path === '/reindex' && (method === 'POST' || method === 'PUT')) {
        await fts.reindex(table, columns, language)
        json(res, 200, { success: true, message: 'Reindex completed' })
        return
      }

      if (path === '/cache/invalidate' && method === 'POST') {
        const body = await parseBody(req)
        const pattern = (body.pattern as string) || undefined
        await fts.invalidateCache(pattern)
        json(res, 200, { success: true, message: 'Cache invalidated' })
        return
      }

      if (path === '/health' || path === '/') {
        json(res, 200, {
          status: 'ok',
          name: 'ftssearch',
          version: '1.0.0',
          uptime: process.uptime(),
        })
        return
      }

      json(res, 404, { success: false, message: `Not found: ${method} ${path}` })
    } catch (err: unknown) {
      const message = err instanceof Error ? err.message : 'Internal server error'
      console.error('[Ftssearch] Server error:', err)
      json(res, 500, { success: false, message })
    }
  })

  return new Promise((resolve) => {
    server.listen(port, host, () => {
      console.log(`[Ftssearch] API server listening on http://${host}:${port}`)
      resolve({ close: () => server.close() })
    })
  })
}
