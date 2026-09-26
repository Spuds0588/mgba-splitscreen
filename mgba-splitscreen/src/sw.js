// v2: ships the online-beta menu label, the crash reporter, and its rescue page.
// Bump whenever the shell file list or their cached contents change, or
// installed PWAs keep serving the stale shell forever.
const CACHE = 'mgba-splitscreen-shell-v2';
const SHELL = [
  './',
  './index.html',
  './styles.css',
  './main.js',
  './online.js',
  './crash-report.js',
  './manifest.json',
  './icon.svg',
];

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(SHELL)));
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
