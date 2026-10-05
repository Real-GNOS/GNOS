// Boot the built game far enough to prove it finds its WADs inside the
// preloaded virtual filesystem.
//
// The only thing missing in Node is a DOM: emscripten's SDL1 shim touches
// document/window unconditionally in _SDL_Init, which runs from main() before
// D_DoomMain().  So we install minimal stubs from Module.preRun - after the
// script body has evaluated, which means ENVIRONMENT_IS_WEB / isNode are
// already fixed and the .data package is still read with fs.readFileSync.
//
// main() then runs IdentifyVersion() -> W_Init() and stops once it needs a
// real canvas.  Reaching W_Init is the proof: IdentifyVersion calls
// I_Error("IWAD not found") otherwise, and I_Error exits before graphics init.
//
//   node run_app.js [doom1|doom2] [site-dir]

'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const GAME = process.argv[2] || 'doom1';
const SITE = process.argv[3] || path.join(__dirname, '..', 'build', 'test_site');

const MARKERS = [
    ['IdentifyVersion: IWAD not found', 'FAIL: no IWAD inside the package'],
    ['Unknown Game Version',            'PASS: IWAD identified (unusual version)'],
    ['W_Init: Init WADfiles.',          'PASS: past W_Init -> IWAD + prboom.wad loaded from the virtual FS'],
    ['V_Init: allocate screens.',       'PASS: past V_Init'],
    ['I_Init: Setting up machine state.','PASS: past I_Init'],
    ['I_InitGraphics',                  'PASS: reached graphics init (browser required from here)'],
];

let marker = 'did not get far enough';
let done = false;

function finish(note) {
    if (done) return;
    done = true;
    if (note) console.log('*** ' + note);
    console.log('\n=> ' + marker);
    process.exit(marker.startsWith('PASS') ? 0 : 1);
}

function note(line) {
    for (const [m, msg] of MARKERS) {
        if (line.includes(m)) { console.log('>>> ' + msg); marker = msg; }
    }
}

const noop = () => {};

globalThis.Module = {
    print: (s) => { note(s); console.log('OUT |', s); },
    printErr: (s) => { note(s); console.log('ERR |', s); },
    onAbort: (why) => { marker = 'FAIL: aborted - ' + why; finish('aborted: ' + why); },
    preRun: [() => {
        // Enough DOM for _SDL_Init's event listeners and Browser's lookups.
        // Anything needing a real canvas fails later, which is fine.
        globalThis.window = {
            addEventListener: noop, removeEventListener: noop,
            location: { pathname: '/', href: 'http://localhost/' },
            devicePixelRatio: 1, innerWidth: 640, innerHeight: 480,
            requestAnimationFrame: noop, cancelAnimationFrame: noop,
        };
        const fakeCanvas = {
            width: 640, height: 480, style: {},
            addEventListener: noop, removeEventListener: noop,
            getContext: () => null, focus: noop, requestPointerLock: noop,
        };
        globalThis.document = {
            addEventListener: noop, removeEventListener: noop,
            createElement: (t) => (t === 'canvas' ? fakeCanvas : { style: {} }),
            getElementById: (id) => (id === 'canvas' ? fakeCanvas : null),
            querySelector: () => null, querySelectorAll: () => [],
            body: { appendChild: noop, removeChild: noop, style: {} },
            exitFullscreen: noop, fullscreenElement: null,
        };
        Module.canvas = fakeCanvas;
    }],
};

globalThis.require = require;
globalThis.__dirname = SITE;
globalThis.__filename = path.join(SITE, GAME + '.js');

process.on('uncaughtException', (e) => {
    console.log('*** stopped at: ' + (e && (e.constructor ? e.constructor.name : '?')) +
                ' - ' + (e && e.message));
    finish();
});

process.chdir(SITE);
try {
    vm.runInThisContext(fs.readFileSync(globalThis.__filename, 'utf8'),
                        { filename: globalThis.__filename });
} catch (e) {
    finish('load error: ' + (e && e.message));
}

setTimeout(() => finish('timeout, still running'), 20000);
