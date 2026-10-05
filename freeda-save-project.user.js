// ==UserScript==
// @name         FreeDa Save Project — 秒哒项目免费备份
// @namespace    https://github.com/freeda/miaoda-save
// @version      1.0.0
// @description  免费保存秒哒(Miaoda)项目：读取 sandbox 目录树，批量拉取文件，用 JSZip 打包下载为 ZIP。同源请求携带全部 Cookie。
// @author       FreeDa
// @match        https://www.miaoda.cn/*
// @require      https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js
// @run-at       document-idle
// @noframes
// @grant        none
// ==/UserScript==

(function () {
  'use strict';

  /* ==================================================================== *
   *  FreeDa Save Project
   *  1. 从当前 URL 取 app-xxxx（也可手动改）
   *  2. GET /api/miaoda/app/sandbox/directory?path=/workspace/app-xxxx  递归列目录
   *  3. GET /api/miaoda/app/sandbox/file?path=...                      批量取文件
   *  4. JSZip 按原目录结构打包 -> 下载 ZIP
   *  所有请求 credentials:'include'，带上页面同款 x-app-id 等头。
   * ==================================================================== */

  const CFG = {
    dirApi: '/api/miaoda/app/sandbox/directory',
    fileApi: '/api/miaoda/app/sandbox/file',
    wsRoot: '/workspace',
    dirConcurrency: 4,
    fileConcurrency: 6,
    retries: 3,
    retryDelay: 600,
    maxPages: 500,          // 分页保护
    maxLogLines: 500,
    defaultExclude: 'node_modules,.git,.svn,.hg,.turbo,.cache,coverage,__pycache__,.venv,venv,.pnpm-store',
  };

  const state = {
    appId: null,
    running: false,
    cancel: false,
    // 扫描阶段
    dirsDone: 0,
    dirsSeen: 0,
    files: [],             // 待下载的相对路径列表
    // 下载阶段
    fileOk: 0,
    fileFail: [],
    zip: null,
    exclude: new Set(CFG.defaultExclude.split(',').map((s) => s.trim()).filter(Boolean)),
    dumpedRoot: false,     // 只 dump 一次根目录响应，window.FDSP_DEBUG=true 时全量
    dumpedFile: false,
  };

  /* ------------------------- 小工具 ------------------------- */

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  function log(msg, level) {
    const el = document.getElementById('fdsp-log');
    const line = `[${new Date().toLocaleTimeString()}] ${msg}`;
    // 始终进 console，方便排查
    if (level === 'error') console.error('[FreeDa]', msg);
    else console.log('[FreeDa]', msg);
    if (!el) return;
    const div = document.createElement('div');
    div.className = 'fdsp-log-' + (level || 'info');
    div.textContent = line;
    el.appendChild(div);
    while (el.childNodes.length > CFG.maxLogLines) el.removeChild(el.firstChild);
    el.scrollTop = el.scrollHeight;
  }

  function setStatus(text) {
    const el = document.getElementById('fdsp-status');
    if (el) el.textContent = text;
  }

  function setProgress(pct, indeterminate) {
    const el = document.getElementById('fdsp-fill');
    if (!el) return;
    if (indeterminate) {
      el.style.width = '100%';
      el.classList.add('fdsp-indet');
    } else {
      el.classList.remove('fdsp-indet');
      el.style.width = Math.max(0, Math.min(100, pct)) + '%';
    }
  }

  async function withRetry(fn, what) {
    let lastErr;
    for (let i = 0; i < CFG.retries; i++) {
      if (state.cancel) throw new Error('已取消');
      try {
        return await fn();
      } catch (e) {
        lastErr = e;
        if (i < CFG.retries - 1) {
          log(`${what} 第 ${i + 1} 次失败: ${e.message}，重试…`, 'warn');
          await sleep(CFG.retryDelay * (i + 1));
        }
      }
    }
    throw lastErr;
  }

  /* ------------------------- app id ------------------------- */

  function detectAppId() {
    const re = /(?:^|[^a-z0-9-])(app-[a-z0-9][a-z0-9-]{3,})(?=$|[^a-z0-9-])/i;
    const pick = (s) => {
      if (!s) return null;
      const m = String(s).match(re);
      return m ? m[1] : null;
    };

    let id = pick(location.pathname) || pick(location.href);
    if (id) return id;

    try {
      const u = new URL(location.href);
      for (const [, v] of u.searchParams) {
        id = pick(v);
        if (id) return id;
      }
    } catch (_) { /* noop */ }

    // 兜底：页面已发出的请求 URL 里往往带 /workspace/app-xxxx
    try {
      for (const r of performance.getEntriesByType('resource')) {
        id = pick(r.name);
        if (id) return id;
      }
    } catch (_) { /* noop */ }

    return null;
  }

  function rootPath() {
    return `${CFG.wsRoot}/${state.appId}`;
  }

  /* ------------------------- HTTP ------------------------- */

  function apiHeaders() {
    const h = {
      'accept': 'application/json, text/plain, */*',
      'x-app-id': state.appId,
      'x-lang': 'zh-CN',
      'x-request-by': 'MIAODA',
      'x-space-id': 'personal',
      'x-requested-with': 'XMLHttpRequest',
    };
    const m = document.cookie.match(/(?:^|;\s*)csrftoken=([^;]*)/);
    if (m && m[1]) h['x-csrftoken'] = decodeURIComponent(m[1]);
    return h;
  }

  // 关键：credentials:'include' —— 带上全部 Cookie（同源本来就带，显式声明更保险）
  async function apiGet(url, what) {
    const res = await fetch(url, {
      method: 'GET',
      credentials: 'include',
      cache: 'no-store',
      headers: apiHeaders(),
    });
    const ct = (res.headers.get('content-type') || '').toLowerCase();
    if (ct.includes('json')) {
      let j = null;
      try { j = await res.json(); } catch (e) { throw new Error(`响应 JSON 解析失败: ${e.message}`); }
      if (!res.ok || (j && typeof j.status === 'number' && j.status >= 400)) {
        throw new Error(`HTTP ${res.status}: ${(j && (j.message || j.msg)) || '请求失败'}`);
      }
      return { json: j, raw: null, httpOk: res.ok };
    }
    const text = await res.text();
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${text.slice(0, 160)}`);
    try {
      return { json: JSON.parse(text), raw: text, httpOk: true };
    } catch (_) {
      return { json: null, raw: text, httpOk: true };
    }
  }

  function dirUrl(path, page) {
    let u = `${CFG.dirApi}?path=${encodeURIComponent(path)}`;
    if (page > 1) u += `&page=${page}&pageSize=500`;
    return u;
  }

  function fileUrl(path) {
    return `${CFG.fileApi}?path=${encodeURIComponent(path)}`;
  }

  /* ------------------------- 目录响应解析（自适应） ------------------------- *
   * 秒哒接口的成功响应结构未公开文档化，这里兼容常见几种外壳与条目字段，
   * 首个响应会原样打进 console（window.FDSP_DEBUG = true 可看完整 dump）。
   * ------------------------------------------------------------------------ */

  function unwrap(j) {
    // 去掉 {status, code, message, data/result/payload} 这类外壳
    let cur = j;
    for (let i = 0; i < 5; i++) {
      if (cur && typeof cur === 'object' && !Array.isArray(cur)) {
        const next = ['data', 'result', 'payload', 'body', 'content'].find(
          (k) => cur[k] != null && typeof cur[k] === 'object'
        );
        if (next) { cur = cur[next]; continue; }
      }
      break;
    }
    return cur;
  }

  // 返回 [{arr, hint}]，hint: 'any' | 'file' | 'dir'
  function collectLists(node) {
    const out = [];
    const push = (arr, hint) => { if (Array.isArray(arr)) out.push({ arr, hint }); };
    if (Array.isArray(node)) { push(node, 'any'); return out; }
    if (!node || typeof node !== 'object') return out;

    push(node.entries, 'any');
    push(node.list, 'any');
    push(node.items, 'any');
    push(node.children, 'any');
    push(node.files, 'file');
    push(node.dirs, 'dir');
    push(node.directories, 'dir');
    push(node.folders, 'dir');

    if (!out.length) {
      // data 里可能是 { files: { list: [...] } } 这类再套一层
      for (const k of ['data', 'result', 'payload']) {
        if (node[k] && typeof node[k] === 'object') {
          const sub = collectLists(node[k]);
          if (sub.length) { out.push(...sub); break; }
        }
      }
    }
    return out;
  }

  const cleanName = (s) =>
    String(s).trim().replace(/^\.?\//, '').replace(/\/+$/, '');

  const joinPath = (parent, name) =>
    (parent.replace(/\/+$/, '') + '/' + String(name).replace(/^\/+/, '')).replace(/\/{2,}/g, '/');

  // 沙箱绝对路径 -> 相对项目根路径（zip 内路径 / 下载队列都用它）
  const relOf = (full) => {
    const root = rootPath();
    if (full === root) return '';
    if (full.startsWith(root + '/')) return full.slice(root.length + 1);
    return String(full).replace(/^\/+/, '');
  };

  function resolveFullPath(parent, name, rawPath) {
    if (rawPath && typeof rawPath === 'string') {
      const p = rawPath.trim();
      if (p.startsWith('/')) {
        // 已经是沙箱绝对路径 / workspace 内绝对路径 / 根相对路径
        if (p.startsWith(CFG.wsRoot + '/')) return p.replace(/\/{2,}/g, '/');
        return joinPath(rootPath(), p.replace(/^\/+/, ''));
      }
      return joinPath(parent, p);
    }
    return joinPath(parent, name);
  }

  function normalizeEntry(item, parentPath, hint) {
    if (item == null) return null;

    if (typeof item === 'string') {
      const name = cleanName(item);
      if (!name || name === '.' || name === '..') return null;
      return {
        name,
        full: joinPath(parentPath, name),
        isDir: hint === 'dir',
        size: null,
        children: null,
      };
    }
    if (typeof item !== 'object') return null;

    let name = item.name ?? item.filename ?? item.fileName ?? item.basename ?? item.title;
    const rawPath = item.path ?? item.fullPath ?? item.absolutePath ?? item.filePath;
    if (!name && typeof rawPath === 'string') {
      const segs = rawPath.split('/').filter(Boolean);
      name = segs.pop();
    }
    if (!name) return null;
    name = cleanName(name);
    if (!name || name === '.' || name === '..') return null;

    const type = String(item.type ?? item.kind ?? item.entryType ?? item.fileType ?? '').toLowerCase();
    let isDir;
    if (item.isDir === true || item.isDirectory === true || item.dir === true || item.folder === true) isDir = true;
    else if (item.isDir === false || item.isDirectory === false || item.dir === false || item.folder === false) isDir = false;
    else if (['dir', 'directory', 'folder', 'd', 'subdir'].includes(type)) isDir = true;
    else if (['file', 'f', 'regular', 'blob', 'application'].includes(type)) isDir = false;
    else if (Array.isArray(item.children)) isDir = true;
    else if (typeof rawPath === 'string' && /\/$/.test(rawPath)) isDir = true;
    else if (hint === 'dir') isDir = true;
    else if (hint === 'file') isDir = false;
    else isDir = false;

    const children = Array.isArray(item.children) ? item.children : null;
    const size = typeof item.size === 'number' ? item.size
      : typeof item.fileSize === 'number' ? item.fileSize
      : typeof item.length === 'number' && !isDir ? item.length : null;

    return {
      name,
      full: resolveFullPath(parentPath, name, typeof rawPath === 'string' ? rawPath : null),
      isDir,
      size,
      children,
    };
  }

  function parseDirectory(resp, parentPath, isRoot) {
    if (resp.json == null) {
      throw new Error(`目录响应不是 JSON: ${String(resp.raw).slice(0, 140)}`);
    }
    const wantDump = (isRoot && !state.dumpedRoot) || window.FDSP_DEBUG === true;
    if (wantDump) {
      state.dumpedRoot = true;
      try {
        console.log(`[FreeDa] 目录响应原始结构 (${parentPath}):`, JSON.parse(JSON.stringify(resp.json)));
      } catch (_) {
        console.log('[FreeDa] 目录响应原始对象:', resp.json);
      }
    }

    const lists = collectLists(unwrap(resp.json));
    if (!lists.length) {
      // 可能整个就是一棵树 {path, children} 或空目录
      const node = unwrap(resp.json);
      if (node && Array.isArray(node)) lists.push({ arr: node, hint: 'any' });
      else if (node && typeof node === 'object' && node.name && (node.isDir || node.children)) {
        lists.push({ arr: [node], hint: 'any' });
      } else {
        log(`目录 ${parentPath} 未解析出条目（可能是空目录），响应: ${JSON.stringify(resp.json).slice(0, 200)}`, 'warn');
        return [];
      }
    }

    const result = [];
    const seen = new Set();
    const stack = [];
    for (const { arr, hint } of lists) {
      for (const it of arr) {
        const e = normalizeEntry(it, parentPath, hint);
        if (e) stack.push(e);
      }
    }

    while (stack.length) {
      const e = stack.pop();
      if (seen.has(e.full)) continue;
      seen.add(e.full);
      if (e.children && e.children.length) {
        // 条目自带 children：直接内联展开，不必再请求
        e.inlined = true;
        for (const c of e.children) {
          const k = normalizeEntry(c, e.full, 'any');
          if (k && !seen.has(k.full)) stack.push(k);
        }
      }
      result.push(e);
    }
    return result;
  }

  /* ------------------------- 并发池 ------------------------- */

  function makePool(limit) {
    const queue = [];
    let active = 0;
    const idleWaiters = [];
    const checkIdle = () => {
      if (!active && !queue.length) {
        while (idleWaiters.length) idleWaiters.shift()();
      }
    };
    const pump = () => {
      while (active < limit && queue.length) {
        const task = queue.shift();
        active++;
        Promise.resolve()
          .then(task)
          .catch(() => { /* 任务内部自己兜错 */ })
          .finally(() => {
            active--;
            pump();
            checkIdle();
          });
      }
      checkIdle();
    };
    return {
      add(fn) { queue.push(fn); pump(); },
      idle() {
        return new Promise((res) => {
          if (!active && !queue.length) res();
          else idleWaiters.push(res);
        });
      },
    };
  }

  /* ------------------------- 阶段一：递归扫描目录 ------------------------- */

  async function scanTree() {
    const queue = [rootPath()];
    const seenDirs = new Set([rootPath()]);
    const files = [];
    const seenFiles = new Set();
    const dirsAll = new Set();
    const errors = [];
    let qi = 0;

    async function worker() {
      while (qi < queue.length && !state.cancel) {
        const dir = queue[qi++];
        try {
          let page = 1;
          let guard = 0;
          do {
            const resp = await withRetry(
              () => apiGet(dirUrl(dir, page), `列目录 ${dir}`),
              `列目录 ${dir}`
            );
            const isRoot = page === 1 && dir === rootPath() && state.dirsDone === 0;
            const entries = parseDirectory(resp, dir, isRoot);
            for (const e of entries) {
              if (e.isDir) {
                // 自带 children 的条目已内联展开，无需再请求；其余目录入队
                if (excluded(e.name)) {
                  // 跳过：不入队，也不在 zip 里留占位目录
                } else {
                  dirsAll.add(relOf(e.full));
                  if (!e.inlined && !seenDirs.has(e.full)) {
                    seenDirs.add(e.full);
                    queue.push(e.full);
                  }
                }
              } else if (!seenFiles.has(e.full)) {
                seenFiles.add(e.full);
                const rel = relOf(e.full);
                if (rel) files.push(rel);   // 存相对路径，下载时再拼回根
              }
            }
            // 分页支持（响应里带 total / hasMore 时才继续）
            const node = unwrap(resp.json) || {};
            const total = typeof node.total === 'number' ? node.total
              : typeof node.totalCount === 'number' ? node.totalCount : null;
            const more = node.hasMore === true || node.has_more === true || node.more === true;
            guard++;
            if (more || (total != null && seenFiles.size + dirsAll.size < total && entries.length >= 100)) {
              page++;
            } else {
              page = 0;
            }
          } while (page > 0 && page <= CFG.maxPages && !state.cancel && ++guard < CFG.maxPages);
        } catch (e) {
          errors.push({ dir, error: e.message });
          log(`目录失败 ${dir}: ${e.message}`, 'error');
        }
        state.dirsDone++;
        state.dirsSeen = seenDirs.size;
        state.files = files;
        setStatus(`扫描目录中… 已扫描 ${state.dirsDone}/${queue.length}，发现文件 ${files.length} 个`);
        setProgress(0, true);
      }
    }

    await Promise.all(
      Array.from({ length: CFG.dirConcurrency }, () => worker())
    );

    return { files, dirs: dirsAll, errors };
  }

  function excluded(name) {
    const n = String(name || '').replace(/\/+$/, '');
    if (!n) return false;
    return state.exclude.has(n);
  }

  // 从 UI 同步排除列表（UI 缺失时保持现有 state.exclude）
  function syncExcludeFromUI() {
    const el = document.getElementById('fdsp-dirs');
    if (!el || typeof el.value !== 'string') return;
    const list = el.value.split(',').map((s) => s.trim()).filter(Boolean);
    state.exclude = new Set(list);
  }

  /* ------------------------- 阶段二：批量下载文件 ------------------------- */

  function extractContent(j) {
    // -> {kind:'text'|'base64'|'bytes', value, truncated}
    const encFlag = (o) => {
      if (!o || typeof o !== 'object') return false;
      const enc = String(o.encoding || o.encode || '').toLowerCase();
      return o.base64 === true || o.isBase64 === true || enc === 'base64';
    };
    const trunc = (a, b) =>
      !!(a && (a.truncated === true || a.isTruncated === true)) ||
      !!(b && (b.truncated === true || b.isTruncated === true));

    // v: 内容值, o: 提供 encoding/truncated 的容器（可能是 v 自己）
    const fromValue = (v, o) => {
      if (v == null) return null;
      if (typeof v === 'string') {
        return { kind: encFlag(o) ? 'base64' : 'text', value: v, truncated: trunc(o) };
      }
      if (Array.isArray(v) && v.length && v.every((x) => typeof x === 'number')) {
        return { kind: 'bytes', value: Uint8Array.from(v), truncated: false };
      }
      if (typeof v === 'object') {
        if (v.type === 'Buffer' && Array.isArray(v.data)) {
          return { kind: 'bytes', value: Uint8Array.from(v.data), truncated: false };
        }
        for (const k of ['content', 'text', 'value', 'source', 'code', 'data']) {
          const s = v[k];
          if (typeof s === 'string') {
            return {
              kind: encFlag(o) || encFlag(v) ? 'base64' : 'text',
              value: s,
              truncated: trunc(o, v),
            };
          }
        }
      }
      return null;
    };

    const nodes = [j];
    if (j && typeof j === 'object') {
      nodes.push(j.data, j.result, j.payload, j.content);
      if (j.data && typeof j.data === 'object') nodes.push(j.data.data);
    }

    for (const n of nodes) {
      if (n == null) continue;
      // 容器自己就是内容
      const direct = fromValue(n, n);
      if (direct) return direct;
      // 常见外壳：{data:{content,encoding}} / {data:{text}}
      for (const k of ['data', 'content', 'text', 'value', 'source', 'code', 'result', 'payload']) {
        if (n[k] == null || n[k] === n) continue;
        const r = fromValue(n[k], n);
        if (r) return r;
      }
    }
    return null;
  }

  async function fetchOneFile(relPath) {
    const full = joinPath(rootPath(), relPath);
    const resp = await withRetry(() => apiGet(fileUrl(full), `取文件 ${relPath}`), `取文件 ${relPath}`);
    if (resp.json == null) {
      // 不是 JSON：整段就是文件内容（text/plain）
      return { kind: 'text', value: resp.raw || '', truncated: false };
    }
    if (!state.dumpedFile || window.FDSP_DEBUG === true) {
      state.dumpedFile = true;
      try {
        console.log(`[FreeDa] 文件响应原始结构 (${relPath}):`, JSON.parse(JSON.stringify(resp.json)));
      } catch (_) { /* noop */ }
    }
    const c = extractContent(resp.json);
    if (!c) {
      throw new Error('未识别的内容字段: ' + JSON.stringify(resp.json).slice(0, 160));
    }
    return c;
  }

  async function downloadFiles(files) {
    const pool = makePool(CFG.fileConcurrency);

    for (const rel of files) {
      pool.add(async () => {
        if (state.cancel) return;
        try {
          const c = await fetchOneFile(rel);
          const zipPath = rel.replace(/^\/+/, '');
          if (c.kind === 'text') state.zip.file(zipPath, c.value);
          else if (c.kind === 'base64') state.zip.file(zipPath, c.value, { base64: true });
          else state.zip.file(zipPath, c.value);
          if (c.truncated) log(`警告: ${rel} 内容被服务端截断`, 'warn');
          state.fileOk++;
        } catch (e) {
          state.fileFail.push({ rel, error: e.message });
          log(`文件失败 ${rel}: ${e.message}`, 'error');
        } finally {
          const done = state.fileOk + state.fileFail.length;
          const pct = files.length ? (done / files.length) * 100 : 100;
          setStatus(`下载文件中… ${done}/${files.length}（失败 ${state.fileFail.length}）`);
          setProgress(pct, false);
        }
      });
    }
    await pool.idle();
  }

  /* ------------------------- 阶段三：打包 + 下载 ------------------------- */

  async function packAndDownload(scan) {
    setStatus('打包 ZIP…');
    const manifest = {
      app: state.appId,
      root: rootPath(),
      savedAt: new Date().toISOString(),
      url: location.href,
      fileCount: state.fileOk,
      failed: state.fileFail,
      skippedDirs: (document.getElementById('fdsp-dirs') || {}).value || '',
      note: 'FreeDa Save Project 生成',
    };
    state.zip.file('_freeda-manifest.json', JSON.stringify(manifest, null, 2));

    // 空目录也保留（scan.dirs 已是相对根路径）
    const hasFiles = state.files.map((f) => f.replace(/^\/+/, ''));
    for (const d of scan.dirs) {
      const rel = String(d).replace(/^\/+/, '').replace(/\/+$/, '');
      if (!rel) continue;
      if (!hasFiles.some((f) => f.startsWith(rel + '/'))) {
        state.zip.file(rel + '/', '');
      }
    }

    const blob = await state.zip.generateAsync(
      { type: 'blob', compression: 'DEFLATE', compressionOptions: { level: 6 } },
      (meta) => {
        setStatus(`打包 ZIP… ${meta.percent.toFixed(1)}%`);
        setProgress(meta.percent, false);
      }
    );

    const ts = new Date();
    const pad = (n) => String(n).padStart(2, '0');
    const name = `${state.appId}_${ts.getFullYear()}${pad(ts.getMonth() + 1)}${pad(ts.getDate())}` +
      `_${pad(ts.getHours())}${pad(ts.getMinutes())}${pad(ts.getSeconds())}.zip`;

    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 60000);

    const mb = (blob.size / 1048576).toFixed(2);
    setStatus(`完成 ✔ ${state.fileOk} 个文件 · ${mb} MB · 已下载 ${name}`);
    setProgress(100, false);
    log(`完成: ${name} (${mb} MB, 文件 ${state.fileOk}, 失败 ${state.fileFail.length})`, 'ok');

    if (state.fileFail.length) {
      log('失败列表: ' + state.fileFail.map((f) => f.rel).join(', '), 'warn');
      const btn = document.getElementById('fdsp-copyfail');
      if (btn) btn.hidden = false;
    }
    return name;
  }

  /* ------------------------- 主流程 ------------------------- */

  async function run() {
    if (state.running) return;
    const idInput = document.getElementById('fdsp-appid');
    const appId = (idInput && idInput.value.trim()) || detectAppId();
    if (!appId || !/^app-[a-z0-9-]+$/i.test(appId)) {
      setStatus('未找到 app-xxxx，请在上方手动填入项目 ID');
      log('无法从 URL 推断 app id', 'error');
      return;
    }
    state.appId = appId;
    syncExcludeFromUI();
    state.running = true;
    state.cancel = false;
    state.dirsDone = 0;
    state.dirsSeen = 0;
    state.files = [];
    state.fileOk = 0;
    state.fileFail = [];
    state.dumpedRoot = false;
    state.dumpedFile = false;

    const btnRun = document.getElementById('fdsp-run');
    const btnCancel = document.getElementById('fdsp-cancel');
    btnRun.disabled = true;
    btnCancel.hidden = false;
    document.getElementById('fdsp-copyfail').hidden = true;
    document.getElementById('fdsp-log').innerHTML = '';

    try {
      log(`开始备份 ${state.appId}（根目录 ${rootPath()}，携带全部 Cookie）`);
      if (typeof JSZip === 'undefined') {
        setStatus('加载 JSZip…');
        await ensureJsZip();
      }
      state.zip = new JSZip();

      // 1. 扫描
      setStatus('扫描目录…');
      setProgress(0, true);
      const scan = await scanTree();
      if (state.cancel) throw new Error('已取消');
      log(`目录扫描完成：${scan.dirs.size} 个目录，${scan.files.length} 个文件` +
        (scan.errors.length ? `，${scan.errors.length} 个目录失败` : ''));

      // 2. 下载
      if (scan.files.length) {
        await downloadFiles(scan.files);
      }
      if (state.cancel) throw new Error('已取消');

      // 3. 打包下载
      await packAndDownload(scan);
    } catch (e) {
      setStatus('出错: ' + e.message);
      log('中断: ' + e.message, 'error');
      setProgress(0, false);
    } finally {
      state.running = false;
      btnRun.disabled = false;
      btnCancel.hidden = true;
    }
  }

  function cancelRun() {
    state.cancel = true;
    setStatus('正在取消…');
    log('用户请求取消', 'warn');
  }

  async function ensureJsZip() {
    const urls = [
      'https://unpkg.com/jszip@3.10.1/dist/jszip.min.js',
      'https://cdn.jsdelivr.net/npm/jszip@3.10.1/dist/jszip.min.js',
    ];
    for (const u of urls) {
      try {
        await new Promise((res, rej) => {
          const s = document.createElement('script');
          s.src = u;
          s.onload = res;
          s.onerror = () => rej(new Error('加载失败 ' + u));
          document.head.appendChild(s);
        });
        if (typeof JSZip !== 'undefined') return;
      } catch (_) { /* 换下一个源 */ }
    }
    throw new Error('JSZip 加载失败（CDN 被墙），请手动改 @require');
  }

  /* ------------------------- UI ------------------------- */

  function injectUI() {
    if (document.getElementById('fdsp-root')) return;

    const style = document.createElement('style');
    style.textContent = `
      #fdsp-root{position:fixed;right:16px;bottom:16px;z-index:2147483647;
        font:13px/1.5 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;color:#e6e9ef}
      #fdsp-toggle{width:46px;height:46px;border-radius:50%;border:none;cursor:pointer;
        background:linear-gradient(135deg,#4f8cff,#7a5cff);color:#fff;font-size:20px;
        box-shadow:0 6px 18px rgba(0,0,0,.45)}
      #fdsp-toggle:hover{transform:scale(1.06)}
      #fdsp-panel{position:absolute;right:0;bottom:58px;width:360px;max-height:78vh;display:flex;
        flex-direction:column;background:#161a22;border:1px solid #2b3242;border-radius:12px;
        box-shadow:0 12px 40px rgba(0,0,0,.55);overflow:hidden}
      #fdsp-panel[hidden]{display:none}
      #fdsp-panel header{display:flex;align-items:center;gap:8px;padding:10px 12px;cursor:move;
        background:linear-gradient(135deg,#1f2634,#171b24);border-bottom:1px solid #2b3242;font-weight:600}
      #fdsp-panel header .sp{flex:1}
      #fdsp-panel header button{background:none;border:none;color:#9aa4b8;font-size:16px;cursor:pointer}
      #fdsp-panel header button:hover{color:#fff}
      #fdsp-body{padding:12px;overflow:auto}
      #fdsp-body label{display:block;color:#9aa4b8;margin:6px 0 4px}
      #fdsp-body input[type=text]{width:100%;box-sizing:border-box;background:#0e1117;color:#e6e9ef;
        border:1px solid #2b3242;border-radius:6px;padding:7px 9px;font:12px/1.4 ui-monospace,Menlo,Consolas,monospace}
      #fdsp-body input[type=text]:focus{outline:none;border-color:#4f8cff}
      #fdsp-body .chk{display:flex;align-items:center;gap:6px;color:#9aa4b8;margin-top:8px;cursor:pointer}
      #fdsp-actions{display:flex;gap:8px;margin-top:10px}
      #fdsp-run{flex:1;background:linear-gradient(135deg,#4f8cff,#7a5cff);border:none;color:#fff;
        padding:9px 10px;border-radius:8px;font-weight:600;cursor:pointer}
      #fdsp-run:disabled{opacity:.5;cursor:not-allowed}
      #fdsp-cancel{background:#2b3242;border:none;color:#ffcf6e;padding:9px 10px;border-radius:8px;cursor:pointer}
      #fdsp-bar{height:8px;background:#0e1117;border-radius:99px;margin-top:10px;overflow:hidden}
      #fdsp-fill{height:100%;width:0;background:linear-gradient(90deg,#4f8cff,#7a5cff);
        transition:width .25s;border-radius:99px}
      #fdsp-fill.fdsp-indet{animation:fdspslide 1.1s linear infinite;background:linear-gradient(90deg,#4f8cff,#7a5cff,#4f8cff)}
      @keyframes fdspslide{0%{transform:translateX(-100%)}100%{transform:translateX(100%)}}
      #fdsp-status{margin-top:8px;color:#c7cede;min-height:18px;font-size:12px;word-break:break-all}
      #fdsp-panel details{margin-top:8px;border-top:1px solid #2b3242;padding-top:6px}
      #fdsp-panel summary{cursor:pointer;color:#9aa4b8;font-size:12px}
      #fdsp-log{max-height:180px;overflow:auto;background:#0e1117;border-radius:6px;padding:6px;
        margin-top:6px;font:11px/1.5 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all}
      .fdsp-log-warn{color:#ffcf6e}.fdsp-log-error{color:#ff7b7b}.fdsp-log-ok{color:#67e08b}
      #fdsp-copyfail{margin-top:8px;background:#2b3242;border:none;color:#c7cede;padding:6px 9px;
        border-radius:6px;cursor:pointer;font-size:12px}
      #fdsp-hint{color:#6b7488;font-size:11px;margin-top:8px}
    `;
    document.head.appendChild(style);

    const root = document.createElement('div');
    root.id = 'fdsp-root';
    root.innerHTML = `
      <div id="fdsp-panel">
        <header id="fdsp-header">
          <span>💾 FreeDa Save Project</span>
          <span class="sp"></span>
          <button id="fdsp-hide" title="收起">−</button>
        </header>
        <div id="fdsp-body">
          <label>项目 ID（自动从当前 URL 提取）</label>
          <input type="text" id="fdsp-appid" spellcheck="false" placeholder="app-xxxxxxxx">
          <label class="chk"><input type="checkbox" id="fdsp-skip" checked>
            跳过依赖/缓存目录：</label>
          <input type="text" id="fdsp-dirs" spellcheck="false">
          <div id="fdsp-actions">
            <button id="fdsp-run">📥 备份为 ZIP</button>
            <button id="fdsp-cancel" hidden>取消</button>
          </div>
          <div id="fdsp-bar"><div id="fdsp-fill"></div></div>
          <div id="fdsp-status">就绪</div>
          <button id="fdsp-copyfail" hidden>📋 复制失败文件列表</button>
          <details>
            <summary>日志</summary>
            <div id="fdsp-log"></div>
          </details>
          <div id="fdsp-hint">同源请求携带全部 Cookie（credentials:include）+ x-app-id 等头；
            若解析异常，打开 F12 看 [FreeDa] 打印的原始响应结构反馈给我。</div>
        </div>
      </div>
      <button id="fdsp-toggle" title="FreeDa Save Project">💾</button>
    `;
    document.body.appendChild(root);

    const panel = root.querySelector('#fdsp-panel');
    const toggle = root.querySelector('#fdsp-toggle');
    const hide = root.querySelector('#fdsp-hide');
    toggle.onclick = () => { panel.hidden = !panel.hidden; };
    hide.onclick = () => { panel.hidden = true; };

    const idInput = root.querySelector('#fdsp-appid');
    const dirsInput = root.querySelector('#fdsp-dirs');
    dirsInput.value = CFG.defaultExclude;
    dirsInput.disabled = !root.querySelector('#fdsp-skip').checked;
    root.querySelector('#fdsp-skip').onchange = (e) => {
      dirsInput.disabled = !e.target.checked;
      if (!e.target.checked) dirsInput.value = '';
      else if (!dirsInput.value.trim()) dirsInput.value = CFG.defaultExclude;
      syncExcludeFromUI();
    };
    dirsInput.addEventListener('input', syncExcludeFromUI);

    const detected = detectAppId();
    if (detected) {
      idInput.value = detected;
      idInput.title = '已自动识别，可修改';
    } else {
      root.querySelector('#fdsp-status').textContent = '未识别到项目 ID，请打开项目页面或手动填写';
    }

    root.querySelector('#fdsp-run').onclick = run;
    root.querySelector('#fdsp-cancel').onclick = cancelRun;
    root.querySelector('#fdsp-copyfail').onclick = () => {
      const txt = state.fileFail.map((f) => `${f.rel}\t${f.error}`).join('\n');
      navigator.clipboard.writeText(txt).then(
        () => log('失败列表已复制', 'ok'),
        () => log('复制失败，请手动从日志取', 'warn')
      );
    };

    // 拖动
    dragify(root.querySelector('#fdsp-header'), root);

    // SPA 路由变化时刷新 app id
    const origPush = history.pushState;
    history.pushState = function () {
      origPush.apply(this, arguments);
      const id = detectAppId();
      if (id) idInput.value = id;
    };
    window.addEventListener('popstate', () => {
      const id = detectAppId();
      if (id) idInput.value = id;
    });
  }

  function dragify(handle, root) {
    let sx = 0, sy = 0, ox = 0, oy = 0, dragging = false;
    handle.addEventListener('mousedown', (e) => {
      if (e.target.tagName === 'BUTTON') return;
      dragging = true;
      const r = root.getBoundingClientRect();
      sx = e.clientX; sy = e.clientY; ox = r.right; oy = r.bottom;
      root.style.left = (window.innerWidth - ox) + 'px';
      root.style.top = (window.innerHeight - oy) + 'px';
      root.style.right = 'auto';
      root.style.bottom = 'auto';
      e.preventDefault();
    });
    window.addEventListener('mousemove', (e) => {
      if (!dragging) return;
      root.style.left = Math.max(0, Math.min(window.innerWidth - 60,
        root.getBoundingClientRect().left + e.clientX - sx)) + 'px';
      root.style.top = Math.max(0, Math.min(window.innerHeight - 60,
        root.getBoundingClientRect().top + e.clientY - sy)) + 'px';
      sx = e.clientX; sy = e.clientY;
    });
    window.addEventListener('mouseup', () => { dragging = false; });
  }

  /* ------------------------- 启动 ------------------------- */

  const boot = () => {
    try { injectUI(); }
    catch (e) { console.error('[FreeDa] UI 注入失败', e); }
  };

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }

  // 暴露给控制台调试：window.FreeDaSave.run() 可脚本触发，FDSP_DEBUG=true 打印全部原始响应
  window.FreeDaSave = { run, detectAppId, state, CFG };
})();
