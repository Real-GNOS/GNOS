#!/usr/bin/env node
/*
 * ccwdown.js — Node.js port of ccwdown (ccwtools 0.1.0)
 *
 * Original project: https://gitee.com/yajn-yin/ccwdown
 * Copyright (c) 2024 yajn-yin
 *
 * This program is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *         http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 *
 * What is ported:
 *   - `download` subcommand, complete: CCW internal API lookup, encrypted
 *     .sb3 download, Base64 + AES-256-CBC decryption, project.json recovery
 *     (byte-error fixes + JSON tail trimming), parallel asset download
 *     (CCW CDN -> Scratch CDN fallback, 16 workers), .sb3 (ZIP, Stored)
 *     reassembly.  All output messages and i18n match the Rust original.
 * What is NOT ported (they need Selenium / the ccw-api-rs SDK):
 *   - `post`, `login-ccw`, `cookie-get` subcommands.
 *
 * Requirements: Node.js >= 18 (global fetch).  No npm dependencies.
 *
 * Usage:
 *   node ccwdown.js <creation-oid> [-o <output-dir>] [-L zh|en]
 *   node ccwdown.js download <creation-oid> [-o <output-dir>]
 */

'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const crypto = require('node:crypto');

const VERSION = '0.1.0';
const API_URL = 'https://community-web.ccw.site/creation/detail';
const SCRATCH_CDN = 'https://cdn.assets.scratch.mit.edu';
const CCW_CDN = 'https://m.xiguacity.cn/user_projects_assets';

const API_TIMEOUT_MS = 15000;   // reqwest .timeout(15s) in api.rs
const MAIN_TIMEOUT_MS = 60000;  // reqwest .timeout(60s) in main.rs download()
const ASSET_TIMEOUT_MS = 15000; // reqwest .timeout(15s) in extract.rs download_asset()

/* ------------------------------------------------------------------ i18n -- */

const LANG = { value: 'zh' };
function t(zh, en) { return LANG.value.startsWith('zh') ? zh : en; }
const M = {
  fetching: () => t('获取作品信息中...', 'Fetching creation info...'),
  title:    () => t('标题', 'Title'),
  link:     () => t('链接', 'Link'),
  downloading: () => t('下载中...', 'Downloading...'),
  downloaded:  () => t('已下载', 'Downloaded'),
  decoding:    () => t('解码中...', 'Decoding...'),
  building:    () => t('构建中...', 'Building...'),
  saved:       () => t('已保存', 'Saved'),
  bytes:       () => t('字节', 'bytes'),
  noLink:      () => t('未找到作品链接', 'No project link found'),
  noId:        () => t('无法提取项目ID', 'Cannot extract project ID'),
};

/* ------------------------------------------------------------ base64 ------ */
/* Port of crypto.rs::lenient_b64_decode: whitespace-tolerant, <=2 pad chars,
 * leftover groups of 1..3 chars accepted (the 4n+1 case of project.json). */

function b64CharVal(c) {
  if (c >= 0x41 && c <= 0x5a) return c - 0x41;           // A-Z
  if (c >= 0x61 && c <= 0x7a) return c - 0x61 + 26;      // a-z
  if (c >= 0x30 && c <= 0x39) return c - 0x30 + 52;      // 0-9
  if (c === 0x2b) return 62;                             // +
  if (c === 0x2f) return 63;                             // /
  return null;
}

/* Rust char::is_ascii_whitespace: space, \t, \n, \x0C (FF), \r — NOT \v. */
const ASCII_WS = new Set([0x20, 0x09, 0x0a, 0x0c, 0x0d]);

function lenientB64Decode(input) {
  const inp = Buffer.from(input.filter((b) => !ASCII_WS.has(b)));
  try {
    new TextDecoder('utf-8', { fatal: true }).decode(inp);
  } catch {
    throw new Error('invalid utf8 in base64');
  }
  let end = inp.length;
  while (end > 0 && inp[end - 1] === 0x3d) end--;
  const padCount = inp.length - end;
  if (padCount > 2) throw new Error('too much padding');
  const raw = inp.subarray(0, end);
  if (raw.length === 0) return Buffer.alloc(0);

  const badChar = () => { throw new Error('bad b64 char'); };
  const full = Math.floor(raw.length / 4);
  const rem = raw.length % 4;
  const out = Buffer.alloc(full * 3 + 3);
  let p = 0;

  for (let g = 0; g < full; g++) {
    const a = b64CharVal(raw[g * 4]);
    const b = b64CharVal(raw[g * 4 + 1]);
    const c = b64CharVal(raw[g * 4 + 2]);
    const d = b64CharVal(raw[g * 4 + 3]);
    if (a === null || b === null || c === null || d === null) badChar();
    out[p++] = (a << 2) | (b >> 4);
    out[p++] = ((b << 4) | (c >> 2)) & 0xff;
    out[p++] = ((c << 6) | d) & 0xff;
  }
  if (rem > 0) {
    const off = full * 4;
    const a = b64CharVal(raw[off]);
    const b = rem > 1 ? b64CharVal(raw[off + 1]) : 0;
    const c = rem > 2 ? b64CharVal(raw[off + 2]) : 0;
    if (a === null || b === null || c === null) badChar();
    out[p++] = ((a << 2) | (b >> 4)) & 0xff;
    if (rem > 2) out[p++] = ((b << 4) | (c >> 2)) & 0xff;
  }
  return out.subarray(0, p);
}

/* ---------------------------------------------------------------- crypto -- */
/* Port of crypto.rs::derive_key / decode_sb3. */

function deriveKey(projectId) {
  const raw = 'KzdnFCBRvq3' + projectId;
  const padLen = (4 - (raw.length % 4)) % 4;
  const padded = raw + '='.repeat(padLen);
  const keyFull = lenientB64Decode(Buffer.from(padded, 'ascii'));
  const key = Buffer.alloc(32);
  keyFull.copy(key, 0, 0, Math.min(keyFull.length, 32));
  const iv = Buffer.from(key.subarray(0, 16));
  return { key, iv };
}

/* Rust u8::from_str error messages (surfaced via "Parse error: {e}"). */
function parseU8(s) {
  if (s === '') throw new Error('Parse error: cannot parse integer from empty string');
  if (!/^\+?[0-9]+$/.test(s)) throw new Error('Parse error: invalid digit found in string');
  const n = Number(s);
  if (n > 255) throw new Error('Parse error: number too large to fit in target type');
  return n;
}

function decodeSb3(data, projectId) {
  const binary = lenientB64Decode(data);
  const { key, iv } = deriveKey(projectId);
  const ctLen = binary.length - (binary.length % 16);

  let pt;
  try {
    const d = crypto.createDecipheriv('aes-256-cbc', key, iv);
    pt = Buffer.concat([d.update(binary.subarray(0, ctLen)), d.final()]);
  } catch (e) {
    throw new Error(`Crypto error: AES decrypt failed: ${e.message}`);
  }

  let text;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(pt);
  } catch {
    throw new Error('UTF-8 error: invalid utf-8 sequence');
  }

  const parts = text.split(',');
  const out = Buffer.alloc(parts.length);
  for (let i = 0; i < parts.length; i++) out[i] = parseU8(parts[i]);
  return out;
}

/* ------------------------------------------------------------- project ---- */
/* Port of extract.rs: percent_decode / find_json_end / decode_project_json. */

function percentDecode(data) {
  const hexVal = (c) => {
    if (c >= 48 && c <= 57) return c - 48;
    if (c >= 65 && c <= 70) return c - 55;
    if (c >= 97 && c <= 102) return c - 87;
    return 0; // Rust .to_digit(16).unwrap_or(0)
  };
  const out = Buffer.alloc(data.length);
  let o = 0;
  let i = 0;
  while (i < data.length) {
    if (data[i] === 0x25 && i + 2 < data.length) {
      out[o++] = (hexVal(data[i + 1]) << 4) | hexVal(data[i + 2]);
      i += 3;
    } else {
      out[o++] = data[i];
      i += 1;
    }
  }
  return out.subarray(0, o);
}

function findJsonEnd(data) {
  let depth = 0;
  let inString = false;
  let escape = false;
  for (let i = 0; i < data.length; i++) {
    const b = data[i];
    if (escape) { escape = false; continue; }
    if (b === 0x5c && inString) { escape = true; continue; } // backslash
    if (b === 0x22) { inString = !inString; continue; }      // quote
    if (!inString) {
      if (b === 0x7b) depth += 1;                            // {
      else if (b === 0x7d) {                                 // }
        depth -= 1;
        if (depth === 0) return i + 1;
      }
    }
  }
  return -1;
}

function decodeProjectJson(raw) {
  const fixed = lenientB64Decode(raw);
  if (fixed.length >= 3) {
    // Case A: '%' '7' <wrong> -> force 'B';  Case B: <wrong> '7' 'B' -> force '%'
    if (fixed[0] === 0x25 && fixed[1] === 0x37 && fixed[2] !== 0x42) {
      fixed[2] = 0x42;
    } else if (fixed[0] !== 0x25 && fixed[1] === 0x37 && fixed[2] === 0x42) {
      fixed[0] = 0x25;
    }
  }
  const decoded = percentDecode(fixed);
  const end = findJsonEnd(decoded);
  if (end >= 0) return decoded.subarray(0, end);
  const last = decoded.lastIndexOf(0x7d);
  if (last >= 0) return decoded.subarray(0, last + 1);
  throw new Error('No JSON object found');
}

function getAssetRefs(data) {
  const result = [];
  const targets = Array.isArray(data.targets) ? data.targets : null;
  if (targets) {
    for (const target of targets) {
      for (const list of ['costumes', 'sounds']) {
        const items = Array.isArray(target && target[list]) ? target[list] : null;
        if (items) {
          for (const item of items) {
            const md5 =
              item && typeof item.md5ext === 'string' ? item.md5ext :
              item && typeof item.md5 === 'string' ? item.md5 :
              null;
            if (md5 !== null) result.push(md5);
          }
        }
      }
    }
  }
  return result;
}

/* ------------------------------------------------------------------ zip --- */

let CRC_TABLE = null;
function crc32(buf) {
  if (!CRC_TABLE) {
    CRC_TABLE = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      CRC_TABLE[n] = c;
    }
  }
  let c = -1;
  for (let i = 0; i < buf.length; i++) {
    c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ -1) >>> 0;
}

/* Minimal ZIP reader: just enough to pull one entry out of the decrypted
 * inner archive (project.json).  Sizes come from the central directory. */
function findZipEntry(zipBuf, want) {
  let eocd = -1;
  const lo = Math.max(0, zipBuf.length - 22 - 65535);
  for (let i = zipBuf.length - 22; i >= lo; i--) {
    if (zipBuf.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('invalid Zip archive: Could not find central directory end');

  const total = zipBuf.readUInt16LE(eocd + 10);
  let p = zipBuf.readUInt32LE(eocd + 16);
  for (let n = 0; n < total; n++) {
    if (p + 46 > zipBuf.length || zipBuf.readUInt32LE(p) !== 0x02014b50) {
      throw new Error('invalid Zip archive: invalid central directory');
    }
    const method = zipBuf.readUInt16LE(p + 10);
    const csize = zipBuf.readUInt32LE(p + 20);
    const nameLen = zipBuf.readUInt16LE(p + 28);
    const extraLen = zipBuf.readUInt16LE(p + 30);
    const cmtLen = zipBuf.readUInt16LE(p + 32);
    const localOff = zipBuf.readUInt32LE(p + 42);
    const name = zipBuf.toString('utf8', p + 46, p + 46 + nameLen);
    p += 46 + nameLen + extraLen + cmtLen;
    if (name !== want) continue;

    if (localOff + 30 > zipBuf.length || zipBuf.readUInt32LE(localOff) !== 0x04034b50) {
      throw new Error('invalid Zip archive: invalid local header');
    }
    const lNameLen = zipBuf.readUInt16LE(localOff + 26);
    const lExtraLen = zipBuf.readUInt16LE(localOff + 28);
    const start = localOff + 30 + lNameLen + lExtraLen;
    const comp = zipBuf.subarray(start, start + csize);
    if (method === 0) return Buffer.from(comp);
    if (method === 8) return zlib.inflateRawSync(comp);
    throw new Error(`unsupported compression method ${method}`);
  }
  return null;
}

/* Minimal ZIP writer: Stored (no compression) entries, streamed to an fd so
 * a 162 MB project never needs a second full copy in memory. */
const DOS_DATE = 1 | (1 << 5) | ((1980 - 1980) << 9); // 1980-01-01
const DOS_TIME = 0;                                   // 00:00:00
const EXTERNAL_ATTR = 0o100644 * 65536;               // regular file, rw-r--r--

class ZipWriter {
  constructor(fd) {
    this.fd = fd;
    this.records = [];
    this.offset = 0;
  }

  _w(buf) {
    fs.writeSync(this.fd, buf);
    this.offset += buf.length;
  }

  add(name, data) {
    const nameBuf = Buffer.from(name, 'utf8');
    const crc = crc32(data);
    const entryOff = this.offset;
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); // local file header signature
    lh.writeUInt16LE(20, 4);         // version needed
    lh.writeUInt16LE(0, 6);          // flags
    lh.writeUInt16LE(0, 8);          // method: stored
    lh.writeUInt16LE(DOS_TIME, 10);
    lh.writeUInt16LE(DOS_DATE, 12);
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(data.length, 18);
    lh.writeUInt32LE(data.length, 22);
    lh.writeUInt16LE(nameBuf.length, 26);
    lh.writeUInt16LE(0, 28);         // extra len
    this._w(lh);
    this._w(nameBuf);
    this._w(data);
    this.records.push({ nameBuf, crc, size: data.length, offset: entryOff });
  }

  finish() {
    const cdStart = this.offset;
    for (const r of this.records) {
      const h = Buffer.alloc(46);
      h.writeUInt32LE(0x02014b50, 0); // central directory signature
      h.writeUInt16LE(0x0314, 4);     // made by: unix, 2.0
      h.writeUInt16LE(20, 6);         // version needed
      h.writeUInt16LE(0, 8);          // flags
      h.writeUInt16LE(0, 10);         // method: stored
      h.writeUInt16LE(DOS_TIME, 12);
      h.writeUInt16LE(DOS_DATE, 14);
      h.writeUInt32LE(r.crc, 16);
      h.writeUInt32LE(r.size, 20);
      h.writeUInt32LE(r.size, 24);
      h.writeUInt16LE(r.nameBuf.length, 28);
      h.writeUInt16LE(0, 30);         // extra len
      h.writeUInt16LE(0, 32);         // comment len
      h.writeUInt16LE(0, 34);         // disk start
      h.writeUInt16LE(0, 36);         // internal attrs
      h.writeUInt32LE(EXTERNAL_ATTR, 38);
      h.writeUInt32LE(r.offset, 42);
      this._w(h);
      this._w(r.nameBuf);
    }
    const cdSize = this.offset - cdStart;
    const e = Buffer.alloc(22);
    e.writeUInt32LE(0x06054b50, 0);   // EOCD signature
    e.writeUInt16LE(0, 4);            // disk
    e.writeUInt16LE(0, 6);            // cd disk
    e.writeUInt16LE(this.records.length, 8);
    e.writeUInt16LE(this.records.length, 10);
    e.writeUInt32LE(cdSize, 12);
    e.writeUInt32LE(cdStart, 16);
    e.writeUInt16LE(0, 20);           // comment len
    this._w(e);
  }
}

/* ------------------------------------------------------------------ api --- */
/* Port of api.rs. */

async function fetchBytes(url, timeoutMs, checkStatus) {
  let resp;
  try {
    resp = await fetch(url, { signal: AbortSignal.timeout(timeoutMs), redirect: 'follow' });
  } catch (e) {
    throw new Error(`HTTP request failed: ${e.name === 'TimeoutError' ? 'operation timed out' : e.message}`);
  }
  if (checkStatus && !resp.ok) {
    throw new Error(`HTTP request failed: HTTP ${resp.status} ${resp.statusText}`.trim());
  }
  try {
    return Buffer.from(await resp.arrayBuffer());
  } catch (e) {
    throw new Error(`HTTP request failed: ${e.name === 'TimeoutError' ? 'operation timed out' : e.message}`);
  }
}

async function getCreationDetail(oid) {
  let resp;
  try {
    resp = await fetch(API_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ oid }),
      signal: AbortSignal.timeout(API_TIMEOUT_MS),
    });
  } catch (e) {
    throw new Error(`HTTP request failed: ${e.name === 'TimeoutError' ? 'operation timed out' : e.message}`);
  }

  let j;
  try {
    j = await resp.json();
  } catch {
    throw new Error('HTTP request failed: error decoding response body');
  }

  if (String(j.code) !== '200') {
    throw new Error(`API error: ${j.msg || 'Unknown error'}`);
  }
  const body = j.body;
  if (!body) throw new Error('API error: Empty response');
  const link =
    (typeof body.latestProjectLink === 'string' && body.latestProjectLink) ||
    (body.creationRelease && typeof body.creationRelease.projectLink === 'string' &&
      body.creationRelease.projectLink) ||
    null;
  if (!link) throw new Error('API error: No project link');
  return { title: String(body.title ?? ''), projectLink: link };
}

function extractProjectId(projectLink) {
  const m = /\/([a-f0-9]{32})\.sb3/.exec(projectLink || '');
  return m ? m[1] : null;
}

/* ------------------------------------------------------------- download --- */

function sanitizeFilename(name) {
  return [...String(name).replace(/[\\/*?:\"<>|]/g, '_')].slice(0, 200).join('');
}

async function downloadAssets(unique) {
  const results = new Array(unique.length);
  let next = 0;
  const worker = async () => {
    for (;;) {
      const i = next++;
      if (i >= unique.length) return;
      const m = unique[i];
      let data = null;
      try {
        data = await fetchBytes(`${CCW_CDN}/${m}`, ASSET_TIMEOUT_MS, true);
      } catch {
        try {
          data = await fetchBytes(`${SCRATCH_CDN}/${m}`, ASSET_TIMEOUT_MS, true);
        } catch {
          /* failed on both CDNs — dropped, like filter_map().ok() in Rust */
        }
      }
      if (data) results[i] = data;
    }
  };
  const n = Math.max(1, Math.min(16, unique.length));
  await Promise.all(Array.from({ length: n }, () => worker()));
  return results; // same order as the sorted-unique list (rayon collect order)
}

async function buildSb3(decodedJson, sb3Path) {
  let data;
  try {
    data = JSON.parse(decodedJson.toString('utf8'));
  } catch (e) {
    throw new Error(`JSON parse error: ${e.message}`);
  }

  const refs = getAssetRefs(data);
  const unique = [...new Set(refs)].sort();
  const total = unique.length;

  console.log(`  Downloading ${total} assets...`);
  const results = await downloadAssets(unique);

  let count = 0;
  for (const r of results) if (r) count++;
  console.log(`  Assets: ${count}/${total}`);
  if (count < total) {
    console.log(`  (${total - count} assets failed to download)`);
  }

  try {
    fs.mkdirSync(path.dirname(path.resolve(sb3Path)), { recursive: true });
    const fd = fs.openSync(sb3Path, 'w');
    try {
      const w = new ZipWriter(fd);
      w.add('project.json', decodedJson);
      for (let i = 0; i < results.length; i++) {
        if (results[i]) w.add(unique[i], results[i]);
      }
      w.finish();
    } finally {
      fs.closeSync(fd);
    }
  } catch (e) {
    if (e.message && /^IO error:/.test(e.message)) throw e;
    throw new Error(`IO error: ${e.message}`);
  }
}

/* --------------------------------------------------------------- flow ----- */
/* Port of main.rs::run_download. */

async function runDownload(oid, output) {
  console.log(`[1/4] ${M.fetching()} ${oid}`);
  const detail = await getCreationDetail(oid);
  console.log(`     ${M.title()}: ${detail.title}`);
  console.log(`     ${M.link()}: ${detail.projectLink}`);

  const projectId = extractProjectId(detail.projectLink);
  if (!projectId) throw new Error(`${M.noId()}: ${detail.projectLink}`);
  console.log(`[2/4] Project UUID: ${projectId}`);

  console.log(`[3/4] ${M.downloading()} ${detail.projectLink}`);
  const sb3Data = await fetchBytes(detail.projectLink, MAIN_TIMEOUT_MS, true);
  console.log(`     ${M.downloaded()} ${sb3Data.length} ${M.bytes()}`);

  console.log(`[4/4] ${M.decoding()}...`);
  const zipData = decodeSb3(sb3Data, projectId);

  const projRaw = findZipEntry(zipData, 'project.json');
  if (!projRaw) throw new Error('project.json not found in ZIP');
  const decoded = decodeProjectJson(projRaw);

  const safeTitle = sanitizeFilename(detail.title);
  const sb3Path = output === '.' ? `./${safeTitle}.sb3` : path.join(output, `${safeTitle}.sb3`);
  await buildSb3(decoded, sb3Path);
  console.log(`     ${sb3Path}`);
  console.log(`${M.saved()}: ${sb3Path}`);
}

/* ----------------------------------------------------------------- cli ---- */

const SUBCOMMANDS = {
  download: 1, d: 1,
  post: 1, p: 1,
  'login-ccw': 1, l: 1,
  'cookie-get': 1, cg: 1,
};
const NOT_PORTED = { post: 1, p: 1, 'login-ccw': 1, l: 1, 'cookie-get': 1, cg: 1 };

const USAGE_HINT = 'Usage: ccwdown [OPTIONS] <COMMAND>';

function printHelp() {
  console.log([
    'CCW (ccw.site) tools',
    '',
    'Usage:',
    '  ccwdown <creation-oid> [-o <output-dir>] [-L <lang>]',
    '  ccwdown <command> [arguments] [options]',
    '',
    'Commands:',
    '  download (d)     Download and decode a CCW Scratch project',
    '  post (p)         Upload a post/article to CCW community   [not in JS port]',
    '  login-ccw (l)    Login to CCW and save cookies            [not in JS port]',
    '  cookie-get (cg)  Get cookies from browser (manual login)  [not in JS port]',
    '',
    'Options:',
    '  -o, --output <DIR>   Output directory [default: .]',
    '  -L, --lang <LANG>    Language: zh (中文) or en (English) [default: zh]',
    '  -V, --version        Show version',
    '  -h, --help           Show help',
    '',
    'Examples:',
    '  ccwdown 669e48a3622bd4577dd0c181 -L zh',
    '  ccwdown 669e5988e1534b396b2c202e -o ./projects -L en',
    '',
    `JS port of https://gitee.com/yajn-yin/ccwdown (Mulan PSL v2) — v${VERSION}`,
  ].join('\n'));
}

function parseFail(msg) {
  console.error(`error: ${msg}\n\n${USAGE_HINT}\n\nFor more information, try '--help'.`);
  return 2;
}

async function runCli() {
  if (typeof fetch !== 'function') {
    console.error('Error: Node.js >= 18 is required (global fetch is missing)');
    return 1;
  }

  const argv = process.argv.slice(2);
  let lang = 'zh';
  let output = '.';
  let oid = null;
  let command = null;
  let wantVersion = false;
  let wantHelp = false;

  const positionals = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const needVal = (label) => {
      if (i + 1 >= argv.length) return parseFail(`a value is required for '${label}' but none was supplied`);
      return null;
    };

    if (a === '--') {
      positionals.push(...argv.slice(i + 1));
      break;
    }
    if (a === '-V' || a === '--version') { wantVersion = true; continue; }
    if (a === '-h' || a === '--help') { wantHelp = true; continue; }
    if (a === '-L' || a === '--lang') {
      const err = needVal('-L <LANG>');
      if (err) return err;
      lang = argv[++i]; continue;
    }
    if (a.startsWith('--lang=')) { lang = a.slice('--lang='.length); continue; }
    if (a === '-o' || a === '--output') {
      const err = needVal('-o <OUTPUT>');
      if (err) return err;
      output = argv[++i]; continue;
    }
    if (a.startsWith('--output=')) { output = a.slice('--output='.length); continue; }
    if (a.length > 1 && a[0] === '-') return parseFail(`unexpected argument '${a}' found`);

    // Not-ported subcommands: reject on first sight, before their own
    // arguments get a chance to trip the generic parser above.
    if (command === null && NOT_PORTED[a]) {
      console.error(
        `Error: subcommand '${a}' is not available in this JS port; ` +
        'use the Rust original: https://gitee.com/yajn-yin/ccwdown'
      );
      return 1;
    }
    if (command === null && SUBCOMMANDS[a]) { command = a; continue; }
    if (oid === null && command === null) { command = 'download'; oid = a; continue; } // README form
    if (oid === null && command === 'download') { oid = a; continue; }
    return parseFail(`unexpected argument '${a}' found`);
  }

  if (wantVersion) { console.log(`ccwdown ${VERSION}`); return 0; }
  if (wantHelp) { printHelp(); return 0; }

  LANG.value = lang;

  if (command === null) {
    console.error(`error: 'ccwdown' requires a subcommand but one was not provided\n\n${USAGE_HINT}\n\nFor more information, try '--help'.`);
    return 2;
  }
  if (NOT_PORTED[command]) {
    console.error(
      `Error: subcommand '${command}' is not available in this JS port; ` +
      'use the Rust original: https://gitee.com/yajn-yin/ccwdown'
    );
    return 1;
  }
  if (!oid) {
    console.error(
      "error: the following required arguments were not provided:\n  <OID>\n\n" +
      'Usage: ccwdown download <OID> [-o <OUTPUT>]\n\n' +
      "For more information, try '--help'."
    );
    return 2;
  }

  await runDownload(oid, output);
  return 0;
}

/* --------------------------------------------------------------- exports -- */

module.exports = {
  lenientB64Decode,
  deriveKey,
  decodeSb3,
  percentDecode,
  findJsonEnd,
  decodeProjectJson,
  getAssetRefs,
  sanitizeFilename,
  crc32,
  ZipWriter,
  findZipEntry,
  getCreationDetail,
  extractProjectId,
  runDownload,
};

if (require.main === module) {
  runCli().then(
    (code) => { process.exitCode = code; },
    (e) => {
      console.error(`Error: ${e && e.message ? e.message : e}`);
      process.exitCode = 1;
    }
  );
}
