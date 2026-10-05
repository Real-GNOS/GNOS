// Verify a browser package produced by build.sh, without needing a browser.
//
//   1. the .wasm is a valid WebAssembly module
//   2. every entry in the .js metadata slices out of the .data byte-for-byte
//      equal to the source asset build.sh preloaded
//   3. the runtime boots: wasm compiles+instantiates, then the packager hands
//      every file to the virtual filesystem - we hash those bytes and compare
//      them to the source assets, which closes the loop end-to-end.
//      main() is suppressed (noInitialRun) so no canvas/DOM is required.
//
//   node verify.js [doom1|doom2] [site-dir]

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

const GAME = process.argv[2] || 'doom1';
const SITE = process.argv[3] || path.join(__dirname, '..', 'build', 'test_site');
const ASSETS = path.dirname(SITE);       // webDOOM/build holds the source assets

const sha256 = (b) => crypto.createHash('sha256').update(b).digest('hex');
let problems = 0;

// ---- 1. wasm module ------------------------------------------------------
const wasmBuf = fs.readFileSync(path.join(SITE, GAME + '.wasm'));
const wasmValid = WebAssembly.validate(wasmBuf);
console.log(`[1] wasm ${wasmBuf.length} bytes, valid=${wasmValid}`);
if (!wasmValid) problems++;

// ---- 2. metadata <-> data round-trip ------------------------------------
const js = fs.readFileSync(path.join(SITE, GAME + '.js'), 'utf8');

// Numbers may come out in exponent form (emscripten emits `95955e3` for
// 95955000), so the numeric pattern has to allow an exponent.
const NUM = '(\\d+e\\d+|\\d+)';
const entries = [];
for (const m of js.matchAll(new RegExp(`\\{filename:"([^"]+)",start:${NUM},end:${NUM}\\}`, 'g'))) {
    entries.push({ name: m[1], start: +m[2], end: +m[3] });
}
for (const m of js.matchAll(new RegExp(`\\{start:${NUM},end:${NUM},filename:"([^"]+)"\\}`, 'g'))) {
    entries.push({ name: m[3], start: +m[1], end: +m[2] });
}
entries.sort((a, b) => a.start - b.start);
console.log(`[2] metadata lists ${entries.length} files`);

const data = fs.readFileSync(path.join(SITE, GAME + '.data'));
const last = entries.reduce((a, e) => Math.max(a, e.end), 0);
console.log(`    .data is ${data.length} bytes; last end offset ${last}` +
    `${last === data.length ? ' (fills .data exactly)' : ' *** MISMATCH ***'}`);
if (last !== data.length) problems++;

const srcSha = new Map();   // virtual path -> sha256 of the source asset
let packed = 0, packedBad = 0, packedNoSrc = 0;
for (const e of entries) {
    const slice = data.subarray(e.start, e.end);
    const src = path.join(ASSETS, e.name.replace(/^\//, ''));
    if (!fs.existsSync(src)) {
        if (slice.length !== 0) { console.log(`    packed but no source: ${e.name}`); packedNoSrc++; }
        continue;
    }
    const want = sha256(fs.readFileSync(src));
    srcSha.set(e.name, want);
    if (sha256(slice) !== want) { console.log(`    CONTENT MISMATCH: ${e.name}`); packedBad++; }
    packed++;
}
console.log(`    byte-compared ${packed}/${entries.length}; ` +
            `mismatched=${packedBad} missing-source=${packedNoSrc}`);
if (packedBad || packedNoSrc) problems++;

// ---- 3. boot the runtime and watch the FS writes -------------------------
// The generated script does `var Module = typeof Module != "undefined" ? Module : {}`.
// Requiring it normally would hoist its own `var Module`, hiding ours, so run
// it in global scope instead where our pre-set object is visible.
//
// emscripten exports Module["FS_createDataFile"] while the script body
// evaluates, i.e. before run().  Hooking it from preRun - which the packager
// runs before its own runWithFS, because we install preRun first - lets us
// hash every byte stream actually written into the virtual filesystem.
const written = new Map();   // virtual path -> sha256 of the bytes written

globalThis.Module = {
    noInitialRun: true,          // stop before main(): there is no canvas here
    preRun: [() => {
        const orig = Module['FS_createDataFile'];
        Module['FS_createDataFile'] = function (parent, name, bytes) {
            if (bytes) written.set(String(parent), sha256(bytes));
            return orig.apply(this, arguments);
        };
    }],
    print: (s) => console.log('    [app]', s),
    printErr: (s) => console.log('    [app:err]', s),
    onAbort: (why) => { console.log('    [abort]', why); problems++; finish(); },
    onRuntimeInitialized: () => {
        console.log('[3] runtime initialised: wasm instantiated + data package applied');

        let bad = 0, unknown = 0, same = 0;
        for (const [name, digest] of written) {
            const want = srcSha.get(name);
            if (want === undefined) { unknown++; continue; }
            if (want !== digest) { console.log(`    FS WRITE MISMATCH: ${name}`); bad++; }
            else same++;
        }
        const neverWritten = [...srcSha.keys()].filter((n) => !written.has(n));
        console.log(`    virtual FS received ${written.size} files: ` +
                    `${same} byte-identical to source, ${bad} different, ` +
                    `${unknown} not in metadata`);
        if (neverWritten.length) {
            console.log('    never written: ' + neverWritten.slice(0, 5));
        }
        if (bad || unknown || neverWritten.length) problems++;
        finish();
    },
};

globalThis.require = require;
globalThis.__dirname = SITE;
globalThis.__filename = path.join(SITE, GAME + '.js');

let finished = false;
function finish() {
    if (finished) return;
    finished = true;
    console.log(problems === 0 ? '\nPASS' : `\nFAIL (${problems} problem(s))`);
    process.exit(problems === 0 ? 0 : 1);
}

process.on('uncaughtException', (e) => {
    console.log('    [uncaught]', e && e.message);
    problems++;
    finish();
});

process.chdir(SITE);
try {
    vm.runInThisContext(fs.readFileSync(globalThis.__filename, 'utf8'),
                        { filename: globalThis.__filename });
} catch (e) {
    console.log('    [load error]', e && e.message);
    problems++;
    finish();
}

setTimeout(() => { console.log('FAIL: runtime never initialised (timeout)'); process.exit(1); }, 60000);
