/*
 * articleRepo.ts — per-article version control on top of git.
 *
 * Git shines on text, so this is the version store for articles: every
 * article has its own repository at storage/repos/posts/{slug}/ tracking
 * exactly two files:
 *
 *   post.md   — the article body as plain text (the diffable part)
 *   post.json — metadata (title, cover_image, category, tags, status)
 *
 * The live source of truth stays the posts row in PostgreSQL; git here is
 * a full history of that row.  Save/publish hooks commit a new version,
 * and rollback restores an old commit's files back into the database.
 *
 * All git calls go through execFile with parameterized args (no shell), so
 * a weird slug or commit message can never become an injection.
 */

import { execFile } from 'child_process'
import { promisify } from 'util'
import fs from 'fs/promises'
import path from 'path'

const execFileAsync = promisify(execFile)

const ROOT = process.cwd()
const POSTS_REPOS_ROOT = path.join(ROOT, 'storage', 'repos', 'posts')

export interface ArticleSnapshot {
  title: string
  content: string
  cover_image?: string
  category?: string
  tags?: string[]
  status?: string
}

export interface VersionInfo {
  sha: string
  message: string
  date: string
  tag?: string
}

function gitArgs(repoDir: string, args: string[]): string[] {
  return ['--git-dir', path.join(repoDir, '.git'), '--work-tree', repoDir, ...args]
}

async function git(repoDir: string, args: string[]): Promise<string> {
  const { stdout } = await execFileAsync('git', gitArgs(repoDir, args), {
    maxBuffer: 64 * 1024 * 1024,
  })
  return stdout
}

function repoDirOf(slug: string): string {
  return path.join(POSTS_REPOS_ROOT, slug)
}

async function repoExists(slug: string): Promise<boolean> {
  try {
    await fs.access(path.join(repoDirOf(slug), '.git'))
    return true
  } catch {
    return false
  }
}

/** git init + identity, idempotent. */
export async function ensureRepo(slug: string): Promise<string> {
  const dir = repoDirOf(slug)
  await fs.mkdir(dir, { recursive: true })
  if (!(await repoExists(slug))) {
    await git(dir, ['init', '-b', 'main'])
    await git(dir, ['config', 'user.name', 'Cocokalo Posts'])
    await git(dir, ['config', 'user.email', 'posts@cocokalo.local'])
    await git(dir, ['config', 'commit.gpgsign', 'false'])
  }
  return dir
}

/** Write post.md + post.json into the repo's working tree. */
async function writeSnapshot(slug: string, snapshot: ArticleSnapshot): Promise<void> {
  const dir = repoDirOf(slug)
  const md = `# ${snapshot.title}\n\n${snapshot.content || ''}`.replace(/\r\n/g, '\n')
  await fs.writeFile(path.join(dir, 'post.md'), md, 'utf-8')
  const meta = {
    title: snapshot.title,
    cover_image: snapshot.cover_image || '',
    category: snapshot.category || '',
    tags: snapshot.tags || [],
    status: snapshot.status || 'draft',
  }
  await fs.writeFile(path.join(dir, 'post.json'), JSON.stringify(meta, null, 2), 'utf-8')
}

/** Commit the current snapshot as a new version.  Returns the commit sha. */
export async function commitArticleVersion(
  slug: string,
  snapshot: ArticleSnapshot,
  message: string,
  version: number
): Promise<string> {
  const dir = await ensureRepo(slug)
  await writeSnapshot(slug, snapshot)
  await git(dir, ['add', '-A'])
  const full = `v${version} — ${message}`.trim()
  await git(dir, ['commit', '--allow-empty', '-m', full])
  await git(dir, ['tag', `v${version}`])
  const { stdout } = await execFileAsync('git', gitArgs(dir, ['rev-parse', 'HEAD']), {
    maxBuffer: 1024 * 1024,
  })
  return stdout.trim()
}

/** List versions newest-first. */
export async function listArticleVersions(slug: string): Promise<VersionInfo[]> {
  if (!(await repoExists(slug))) return []
  const dir = repoDirOf(slug)
  const out = await git(dir, [
    'log', '--date=iso-strict', '--pretty=format:%H%x00%ct%x00%s%x00%D',
  ])
  if (!out.trim()) return []
  const versions: VersionInfo[] = []
  for (const line of out.split('\n')) {
    if (!line.trim()) continue
    const [sha, ct, subject, refs] = line.split('\x00')
    const tagMatch = (refs || '').match(/tag: (v\d+)/)
    versions.push({
      sha: sha.trim(),
      date: new Date(parseInt(ct, 10) * 1000).toISOString(),
      message: (subject || '').trim(),
      tag: tagMatch ? tagMatch[1] : undefined,
    })
  }
  return versions
}

/** Read post.md + post.json at a given commit. */
export async function readArticleVersion(
  slug: string,
  sha: string
): Promise<ArticleSnapshot | null> {
  const dir = repoDirOf(slug)
  const mdOut = await git(dir, ['show', `${sha}:post.md`]).catch(() => null)
  const jsonOut = await git(dir, ['show', `${sha}:post.json`]).catch(() => null)
  if (mdOut === null && jsonOut === null) return null

  let meta: any = {}
  if (jsonOut) {
    try {
      meta = JSON.parse(jsonOut)
    } catch {}
  }
  let content = mdOut || ''
  let title = meta.title || ''
  if (mdOut && mdOut.startsWith(`# `)) {
    const nl = mdOut.indexOf('\n')
    const head = nl >= 0 ? mdOut.slice(0, nl) : mdOut
    title = head.replace(/^#\s*/, '').trim()
    content = nl >= 0 ? mdOut.slice(nl + 1) : ''
  }
  return {
    title: title || meta.title || '',
    content,
    cover_image: meta.cover_image || '',
    category: meta.category || '',
    tags: meta.tags || [],
    status: meta.status || 'draft',
  }
}

/** Text diff between two versions, with unified context. */
export async function diffArticleVersions(
  slug: string,
  from: string,
  to: string
): Promise<{ diff: string; summary: string }> {
  const dir = repoDirOf(slug)
  const out = await git(dir, ['diff', '--stat', from, to])
  const diffOut = await git(dir, ['diff', '-U3', from, to, '--', 'post.md', 'post.json'])
  const lines = out.split('\n').filter((l) => l.trim())
  return {
    diff: diffOut,
    summary: lines.length ? lines[lines.length - 1] : '(无差异)',
  }
}

/**
 * Snapshot the article's CURRENT database row into git as a new version
 * and record it in article_versions.  Called by the create/update API so
 * every save is automatically versioned.  Returns {version, sha} or null
 * when the article no longer exists.
 */
export async function commitCurrentPostVersion(
  slug: string,
  message: string,
  operatorId: number,
  operatorName: string
): Promise<{ version: number; sha: string } | null> {
  // lazy import to avoid a hard cycle: posts.ts is loaded by the same server
  const { queryPg } = await import('./pg')
  const post = await queryPg(
    'SELECT title, content, cover_image, category, tags, status FROM posts WHERE slug = $1',
    [slug]
  )
  if (!post.rows.length) return null
  const row = post.rows[0]

  const next = await queryPg(
    'SELECT COALESCE(MAX(version), 0) + 1 AS next FROM article_versions WHERE article_slug = $1',
    [slug]
  )
  const version = parseInt(next.rows[0].next, 10)

  const sha = await commitArticleVersion(
    slug,
    {
      title: row.title,
      content: row.content,
      cover_image: row.cover_image,
      category: row.category,
      tags: row.tags,
      status: row.status,
    },
    message || `保存版本 v${version}`,
    version
  )

  await queryPg(
    `INSERT INTO article_versions (article_slug, version, commit_sha, tag, message, operator_id, operator_name)
     VALUES ($1, $2, $3, $4, $5, $6, $7)`,
    [slug, version, sha, `v${version}`, message || `保存版本 v${version}`, operatorId, operatorName]
  )
  await queryPg(
    `INSERT INTO audit_logs (user_id, action, target_type, target_id, details)
     VALUES ($1, $2, $3, $4, $5)`,
    [operatorId, 'commit_article_version', 'post', slug,
     `文章 ${slug} 提交版本 v${version}：${message}`]
  )
  return { version, sha }
}
