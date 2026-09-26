// v2: ships the online-beta menu label, the crash reporter, and its rescue page.
// Bump whenever the shell file list or their cached contents change, or
// installed PWAs keep serving the stale shell forever.
const CACHE = 'mgba-splitscreen-shell-v3';
const SHELL = [
  './',
  './index.html',
  './styles.css',
  './main.js',
  './online.js',
  './crash-report.js',
  './manifest.json',
  './icon.svg',
  './icon-32.png',
  './icon-192.png',
  './icon-512.png',
  './icon-maskable-192.png',
  './icon-maskable-512.png',
  './apple-touch-icon.png',
];

self.addEventListener('install', (event) => {
  // cache:'reload' bypasses the HTTP cache: GitHub Pages serves max-age=600,
  // and without this a freshly-installed worker could cache up to ten-minute
  // stale shell files right after a deploy, keeping old code alive a cycle.
  event.waitUntil(caches.open(CACHE).then((cache) =>
    cache.addAll(SHELL.map((path) => new Request(path, { cache: 'reload' }))),
  ));
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) => Promise.all(
      keys.filter((key) => key !== CACHE).map((key) => caches.delete(key)),
    )).then(() => self.clients.claim()),
  );
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') return;
  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return;

  // Navigation is network-first so a magic link never opens an obsolete shell.
  if (request.mode === 'navigate') {
    event.respondWith(fetch(request).catch(() => caches.match('./index.html')));
    return;
  }

  // The crash rescue page is URL-parameterized (?report=...); never answer it
  // from a cache that cannot know those parameters.
  if (url.pathname.endsWith('/report-issue.html')) return;

  // Only cache the app shell. ROMs, save files, PeerJS, and WASM remain
  // network-controlled and are never silently persisted by this worker.
  if (SHELL.some((path) => new URL(path, self.location.href).pathname === url.pathname)) {
    event.respondWith(caches.match(request).then((cached) => cached || fetch(request)));
  }
});
