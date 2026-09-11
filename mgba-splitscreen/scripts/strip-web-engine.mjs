// Remove the local in-browser (WASM) engine copies before Tauri bundles the
// desktop app.
//
// Why a script file instead of the one-liner this replaces: Tauri runs
// `beforeBuildCommand` through a shell on Linux/macOS but *directly* (no shell)
// on Windows, so a command like `node -e "const fs=require('fs')..."` reaches
// node with the wrapping double quotes still attached to the argument. Node
// then tries to evaluate a program starting with a literal `"` and dies with
// `SyntaxError: Invalid or unexpected token` before any of it runs — which is
// exactly how the v0.3.0 Windows release job failed while Linux and macOS
// passed. Passing a real file as the argument has no quoting to get wrong.
//
// The desktop bundle embeds ../src, so any `mgba-splitscreen-web.*` sitting
// there (web/build.sh drops them in for local previews; they are gitignored)
// would otherwise be shipped inside the app. The Windows and macOS builds must
// stay wasm-free: the desktop app runs the native Rust backend.
//
// Paths are resolved from this file's own location, not the working directory,
// and a missing file is not an error — in CI the copies exist, locally they
// may not.

import { unlinkSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const appDir = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const srcDir = join(appDir, 'src');

const artifacts = [
  'mgba-splitscreen-web.js',
  'mgba-splitscreen-web.wasm',
  'mgba-splitscreen-web-dbg.js',
  'mgba-splitscreen-web-dbg.wasm',
];

let removed = 0;
for (const name of artifacts) {
  const target = join(srcDir, name);
  if (!existsSync(target)) continue;
  try {
    unlinkSync(target);
    removed++;
  } catch (err) {
    // A read-only or locked file (a running `tauri dev` holds the wasm open on
    // Windows) must not fail the bundle: the engine simply ends up included.
    console.warn(`strip-web-engine: could not remove ${name}: ${err.message}`);
  }
}

console.log(
  `strip-web-engine: removed ${removed} web engine file(s) from ${srcDir} ` +
    `so the desktop bundle stays native-only`,
);
