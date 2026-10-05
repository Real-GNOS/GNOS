/*
 * videoRepo.ts — per-video asset version control on top of git.
 *
 * Every video gets its own git repository at storage/repos/{slug}/
 * (git 2.47.2, invoked through execFile with fully parameterized args — no
 * shell, so a weird slug or message can never turn into an injection).
 * The repository tracks an `assets/` directory: a snapshot of the video's
 * current storage/fmp4/{slug}/ tree (playable 360p/720p/1080p + video.json).
 *
 * Version semantics:
 *   - publish:  snapshot fmp4 -> assets/, git add + commit, tag v{N},
 *               record the row in video_versions (slug, N, sha, message,
 *               operator, created_at).
 *   - rollback: git checkout <sha> -- assets, then copy the tree back over
 *               storage/fmp4/{slug}/ so the live video reverts.
 *   - diff:     git diff --stat between two commits (sizes per file).
 *
 * storage layout (all under the project root, resolved at import time):
 *   storage/repos/{slug}/            the git worktree for this video
 *   storage/repos/{slug}/assets/     tracked snapshot (inside the worktree)
 *   storage/fmp4/{slug}/             live playable assets (not in git)
 */

import { execFile } from 'child_process'
import { promisify } from 'util'
import fs from 'fs/promises'
import path from 'path'

const execFileAsync = promisify(execFile)

const ROOT = process.cwd()
const REPOS_ROOT = path.join(ROOT, 'storage', 'repos')
const FMP4_ROOT = path.join(ROOT, 'storage', 'fmp4')

const GIT_AUTHOR = 'cocokalo-assets <assets@cocokalo.local>'

export interface VersionInfo {
  sha: string
  version?: number
  tag?: string
  message: string
  date: string
}

/** All git calls run with an explicit --git-dir/--work-tree pair so the
 *  process's own CWD (whatever the server sets it to) never matters. */
function gitArgs(repoDir: string, args: string[]): string[] {
  return ['--git-dir', path.join(repoDir, '.git'), '--work-tree', repoDir, ...args]
}

async function git(repoDir: string, args: string[]): Promise<string> {
  const { stdout } = await execFileAsync('git', gitArgs(repoDir, args), {
    maxBuffer: 64 * 1024 * 1024,
  })
  return stdout
}

/** git can be slow to "see" a just-mkdir'd repo; make sure the dir exists
 *  before any git call that expects a worktree. */
async function ensureDir(p: string) {
  await fs.mkdir(p, { recursive: true })
}

/** Is the repository initialized for this slug? */
async function repoExists(slug: string): Promise<boolean> {
  try {
    await fs.access(path.join(REPOS_ROOT, slug, '.git'))
    return true
  } catch {
    return false
  }
}

/** git init + identity, idempotent. */
export async function ensureRepo(slug: string): Promise<string> {
  const repoDir = path.join(REPOS_ROOT, slug)
  await ensureDir(repoDir)
  if (!(await repoExists(slug))) {
    await git(repoDir, ['init', '-b', 'main'])
    await git(repoDir, ['config', 'user.name', 'Cocokalo Assets'])
    await git(repoDir, ['config', 'user.email', 'assets@cocokalo.local'])
    await git(repoDir, ['config', 'commit.gpgsign', 'false'])
  }
  return repoDir
}

/** Copy the live fmp4 tree into the repo's assets/ dir (fresh snapshot). */
async function snapshotToAssets(slug: string, repoDir: string): Promise<void> {
  const src = path.join(FMP4_ROOT, slug)
  const dst = path.join(repoDir, 'assets')
  await fs.rm(dst, { recursive: true, force: true })
  await ensureDir(dst)
  let entries: string[] = []
  try {
    entries = await fs.readdir(src)
  } catch {
    // no fmp4 tree yet — an empty assets/ snapshot is still a valid commit
  }
  for (const name of entries) {
    const s = path.join(src, name)
    const d = path.join(dst, name)
    const st = await fs.stat(s)
    if (st.isDirectory()) {
      await fs.cp(s, d, { recursive: true })
    } else {
      await fs.copyFile(s, d)
    }
  }
}

/** Publish the current fmp4 tree as a new version.  Returns the commit sha. */
export async function publishVersion(
  slug: string,
  message: string,
  version: number
): Promise<string> {
  const repoDir = await ensureRepo(slug)
  await snapshotToAssets(slug, repoDir)
  await git(repoDir, ['add', '-A'])
  const full = `v${version} — ${message}`.trim()
  // allow-empty keeps a snapshot that is byte-identical to the last one from
  // creating an error; that is a legitimate "re-publish" action.
  await git(repoDir, ['commit', '--allow-empty', '-m', full])
  await git(repoDir, ['tag', `v${version}`])
  const { stdout } = await execFileAsync('git', gitArgs(repoDir, ['rev-parse', 'HEAD']), {
    maxBuffer: 1024 * 1024,
  })
  return stdout.trim()
}

/** List commits newest-first, with the tag name if the commit carries one. */
export async function listVersions(slug: string): Promise<VersionInfo[]> {
  if (!(await repoExists(slug))) return []
  const repoDir = path.join(REPOS_ROOT, slug)
  const out = await git(repoDir, [
    'log', '--date=iso-strict', '--pretty=format:%H%x00%ct%x00%s%x00%D',
  ])
  if (!out.trim()) return []
  const lines = out.split('\n')
  const versions: VersionInfo[] = []
  for (const line of lines) {
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

/** Restore the assets/ of a specific commit back over the live fmp4 tree. */
export async function rollbackTo(slug: string, sha: string): Promise<void> {
  const repoDir = path.join(REPOS_ROOT, slug)
  // `git checkout <sha> -- assets` inside the worktree restores that commit's
  // tracked snapshot into assets/; then copy assets/ over the live tree.
  await git(repoDir, ['checkout', sha, '--', 'assets'])
  const src = path.join(repoDir, 'assets')
  const dst = path.join(FMP4_ROOT, slug)
  await fs.rm(dst, { recursive: true, force: true })
  await ensureDir(dst)
  let entries: string[] = []
  try {
    entries = await fs.readdir(src)
  } catch {
    return
  }
  for (const name of entries) {
    const s = path.join(src, name)
    const d = path.join(dst, name)
    const st = await fs.stat(s)
    if (st.isDirectory()) {
      await fs.cp(s, d, { recursive: true })
    } else {
      await fs.copyFile(s, d)
    }
  }
}

/** Human-readable per-file size diff between two commits. */
export async function diffVersions(
  slug: string,
  from: string,
  to: string
): Promise<{ files: string[]; summary: string }> {
  const repoDir = path.join(REPOS_ROOT, slug)
  const out = await git(repoDir, ['diff', '--stat', from, to])
  const lines = out.split('\n').filter((l) => l.trim())
  return { files: lines, summary: lines.length ? lines[lines.length - 1] : '(无差异)' }
}
