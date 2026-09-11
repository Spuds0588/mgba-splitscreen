// Copy the in-browser (WASM) engine into the bundle for Android builds.
//
// Why this exists alongside strip-web-engine.mjs: the two platforms want the
// opposite thing from ../src. The desktop app runs the native Rust core and must
// NOT ship a second emulator, so its beforeBuildCommand deletes these files. The
// Android app *is* the web build inside the system WebView — no native core is
// built for it at all (see build.rs) — so it needs both artifacts present at
// bundle time. tauri.android.conf.json swaps this script in for that reason.
//
// The artifacts are gitignored build output (web/build.sh produces them), so CI
// has to generate them before the Android build runs.
//
// A missing artifact is a hard error on purpose. An APK whose engine is absent
// still installs and still launches, then shows the "could not start its engine"
// notice — a confusing on-device failure to debug from a TV remote. Failing the
// build is much cheaper to diagnose.

import { copyFileSync, existsSync, mkdirSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const appDir = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const srcDir = join(appDir, 'src');
const webDir = join(appDir, 'web');

// name -> minimum plausible size, to catch a truncated or empty artifact rather
// than shipping one that fails at runtime.
const artifacts = [
  { name: 'mgba-splitscreen-web.js', minBytes: 20 * 1024 },
  { name: 'mgba-splitscreen-web.wasm', minBytes: 100 * 1024 },
];

const problems = [];
for (const { name, minBytes } of artifacts) {
  const from = join(webDir, name);
  if (!existsSync(from)) {
    problems.push(`${from} does not exist — run web/build.sh first`);
    continue;
  }
  const size = statSync(from).size;
  if (size < minBytes) {
    problems.push(`${from} is only ${size} bytes (expected at least ${minBytes})`);
    continue;
  }
  mkdirSync(srcDir, { recursive: true });
  copyFileSync(from, join(srcDir, name));
  console.log(`stage-web-engine: ${name} (${size} bytes) -> src/`);
}

if (problems.length) {
  for (const p of problems) console.error(`stage-web-engine: ${p}`);
  console.error(
    'stage-web-engine: refusing to build an Android bundle without the in-browser engine',
  );
  process.exit(1);
}
