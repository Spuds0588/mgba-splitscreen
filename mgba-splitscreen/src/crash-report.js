/* Crash reporter (opt-out): turn uncaught errors into ready-to-file GitHub issues.
 *
 * beta testers should not need devtools to give us useful bug reports. This
 * module listens for uncaught errors and rejected promises, collects the
 * details needed to troubleshoot (game ROM, engine/platform, players, online
 * session state, system info, and the tail of the console log ring buffer),
 * then opens a prefilled issue on the project tracker in a new tab.
 *
 * How it stays useful even for HARD crashes (the WASM engine aborting can
 * leave the page too broken to run this script's UI flow):
 *   1. If the direct popup is blocked, the browser opens
 *      `report-issue.html?report=...` — a static rescue page with its own
 *      tiny script that files the issue from the report embedded in its URL.
 *   2. The report and the ready-to-open issue URL are mirrored to
 *      localStorage, so a rescue page (or any later tab) can still file it.
 *   3. Dedup (sessionStorage + opener check) keeps one issue per report
 *      even when both the main page and the rescue page try to file.
 *
 * Everything is best-effort: a failure to file never breaks gameplay, and the
 * reporter stays out of the way unless something actually goes wrong.
 */
(() => {
  const ISSUES_URL = 'https://github.com/Spuds0588/mgba-splitscreen/issues/new';
  // The Pages workflow replaces this placeholder with the deployed commit SHA
  // so a crash report names the exact build. Unreplaced (local/dev): 'unknown'.
  const GIT_SHA = '__GIT_SHA__';
  const BUILD_SHA = GIT_SHA.startsWith('__') ? 'unknown' : GIT_SHA.slice(0, 8);
  const APP_VERSION = '0.3.0+online-beta';

  const bootedAt = Date.now();
  let lastReport = null;      // full report object, mirrored to localStorage
  let lastIssueUrl = null;    // ready-to-open GitHub issue URL, mirrored likewise
  let lastFingerprint = '';
  let lastFingerprintAt = 0;
  let reportCount = 0;
  const MAX_REPORTS_PER_SESSION = 5; // a crash loop must not open tabs forever

  function query(name) {
    try { return new URLSearchParams(window.location.search).get(name); } catch (_) { return null; }
  }

  function bytesToMB(n) {
    return n ? `${(n / (1024 * 1024)).toFixed(0)} MB` : 'unknown';
  }

  function onlineState() {
    const o = window.mgbaOnline;
    if (!o) return 'not loaded';
    if (o.isGuest && o.isGuest()) {
      const seat = o.assignedPlayer ? o.assignedPlayer() : null;
      return `guest (player ${Number.isInteger(seat) ? seat + 1 : '?'})`;
    }
    if (o.isHost && o.isHost()) {
      const guests = o.guestCount ? o.guestCount() : null;
      return `host (${Number.isInteger(guests) ? guests : '?'} guest(s) connected)`;
    }
    return 'idle';
  }

  // ---- Context collection ---------------------------------------------------

  function systemDetails() {
    const nav = navigator;
    const mem = performance && performance.memory;
    let gpu = 'unknown';
    try {
      const c = document.createElement('canvas');
      const gl = c.getContext('webgl2') || c.getContext('webgl');
      if (gl) {
        const dbg = gl.getExtension('WEBGL_debug_renderer_info');
        gpu = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
      }
    } catch (_) { /* canvases can be blocked; never fatal */ }
    return {
      'User agent': nav.userAgent,
      'Language': nav.language || 'unknown',
      'Platform': nav.platform || 'unknown',
      'CPU cores': nav.hardwareConcurrency || 'unknown',
      'Device memory': nav.deviceMemory ? `${nav.deviceMemory} GB (approx)` : 'unknown',
      'Screen': `${screen.width}x${screen.height} @ ${window.devicePixelRatio}x, window ${innerWidth}x${innerHeight}`,
      'Touch': ('ontouchstart' in window) || (nav.maxTouchPoints > 0) ? 'yes' : 'no',
      'GPU renderer': gpu,
      'Network': nav.connection && nav.connection.effectiveType
        ? nav.connection.effectiveType : 'unknown',
      'Page state': `visible=${document.visibilityState}, browserOnline=${nav.onLine}`,
      'JS heap used': mem ? bytesToMB(mem.usedJSHeapSize) : 'unknown',
      'Time (UTC)': new Date().toISOString(),
    };
  }

  function appDetails() {
    const M = window.wasmModuleRef || null; // set by main.js once the engine loads
    // mgs_get_stats(int out[4]) -> steps, lockstep sleeps, wakes, emulated cycles.
    let stats = '';
    try {
      if (M && M._mgs_get_stats) {
        const out = M._malloc ? M._malloc(16) : 0;
        if (out) {
          M._mgs_get_stats(out);
          const v = new Int32Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + out, 4);
          stats = `steps=${v[0]} linkSleeps=${v[1]} linkWakes=${v[2]} cycles=${v[3]}`;
          M._free(out);
        }
      }
    } catch (_) {}
    // mgs_get_platform(player): -1 none, 0 GBA, 1 GB (GBC boots as GB core).
    let platform = '';
    try {
      if (M && M._mgs_get_platform) {
        const p = M._mgs_get_platform(0);
        platform = { '-1': 'none', 0: 'GBA', 1: 'GB/GBC' }[p] || String(p);
      }
    } catch (_) {}
    return {
      'App version': `${APP_VERSION} (build ${BUILD_SHA})`,
      'Page URL': `${location.origin}${location.pathname}`, // query/hash dropped: invites carry bearer tokens
      'Session uptime': `${Math.round((Date.now() - bootedAt) / 1000)} s`,
      'Referrer': document.referrer || 'none',
      'In-browser engine': (window.wasmModeRef === undefined) ? 'not booted yet' : (window.wasmModeRef ? 'running (WASM)' : 'not running'),
      'Players configured': window.playerCountRef ?? 'unknown',
      'Game ROM': window.lastRunningRom || 'none loaded',
      'Game platform': platform || 'unknown',
      'Engine stats': stats ? String(stats).replace(/\s+/g, ' ').trim() || 'unavailable' : 'unavailable',
      'FS assist': window.fsAssistActiveRef ? 'on' : 'off',
      'Online session': onlineState(),
      'Audio state': (() => {
        try {
          const a = window.audioStateRef;
          return a ? `ctx=${a.ctxState || 'unlocked?'}, resampler=${a.node ? 'ready' : 'missing'}, buffered=${a.buffered} frames @${a.srcRate}Hz` : 'unknown';
        } catch (_) { return 'unknown'; }
      })(),
      'Engine failure banner shown': (() => {
        try { const el = document.getElementById('hosted-note'); return el && !el.hidden ? 'YES' : 'no'; } catch (_) { return 'unknown'; }
      })(),
    };
  }

  function logTail() {
    // main.js mirrors console output into window.sessionLogRef (a string array).
    const log = window.sessionLogRef;
    if (!Array.isArray(log) || !log.length) return '(no console output captured)';
    return log.slice(-80).join('\n');
  }

  // ---- Report assembly + filing ----------------------------------------------

  function collect(kind, message, err) {
    const details = { ...systemDetails(), ...appDetails() };
    let stack = '';
    if (err && err.stack) stack = String(err.stack);
    const report = {
      kind,
      message: String(message || '(no message)'),
      stack,
      loggedAt: new Date().toISOString(),
      details,
      log: logTail(),
    };
    report.body = formatBody(report); // pre-render so the rescue page never needs this module
    return report;
  }

  function formatBody(report) {
    const lines = [];
    lines.push(report.kind === 'Manual report'
      ? 'Manual bug report from the web build (filed by the tester from the in-app menu; the details below were collected automatically at that moment).'
      : `${report.kind} on the web build (reported automatically by the in-app beta reporter).`);
    lines.push('');
    lines.push(`**What failed:** ${report.message}`);
    if (report.stack) {
      lines.push('');
      lines.push('```');
      lines.push(report.stack.split('\n').slice(0, 12).join('\n'));
      lines.push('```');
    }
    lines.push('');
    lines.push('### Game & session');
    for (const [k, v] of Object.entries(report.details)) lines.push(`- **${k}:** ${v}`);
    lines.push('');
    lines.push('### Console log tail');
    lines.push('```');
    lines.push(report.log);
    lines.push('```');
    lines.push('');
    lines.push('<sub>Filed via crash-report.js — thank you for testing the online beta! You can add reproduction steps above.</sub>');
    return lines.join('\n');
  }

  function issueUrl(report) {
    const url = new URL(ISSUES_URL);
    const prefix = report.kind === 'Manual report' ? '[feedback]' : '[auto]';
    url.searchParams.set('title', `${prefix} ${report.kind}: ${report.message.slice(0, 160)}`);
    url.searchParams.set('body', formatBody(report));
    url.searchParams.set('labels', 'crash-report,web,online-beta');
    return url.href;
  }

  function rescueUrl(report) {
    // The static fallback page can still file the issue if this tab dies
    // mid-crash (it reads the report back out of localStorage) or if the
    // browser blocked the direct popup.
    const url = new URL('report-issue.html', location.href);
    url.searchParams.set('report', JSON.stringify(report));
    url.searchParams.set('auto', '1');
    return url.href;
  }

  function fileIssue(report) {
    const url = issueUrl(report);
    // Persist both the report and the ready-to-open issue URL so the rescue
    // page (or a later manual visit) can file without this tab being alive.
    try {
      lastReport = report;
      lastIssueUrl = url;
      localStorage.setItem('mgs:lastCrashReport', JSON.stringify(report));
      localStorage.setItem('mgs:lastCrashIssueUrl', url);
    } catch (_) {}
    let opened = false;
    try { opened = !!window.open(url, '_blank', 'noopener'); } catch (_) {}
    if (!opened) {
      try { window.open(rescueUrl(report), '_blank', 'noopener'); } catch (_) {}
    }
    // Desktops usually let the auto-open through; phones block window.open
    // from an error handler (no user gesture). Either way, take over the
    // screen with a one-tap file button: the tap IS the gesture mobile
    // browsers demand, so filing always works from here.
    if (!showCrashOverlay(report, opened)) showReopenPill();
  }

  // Full-screen crash/report offer. Returns false only if it could not render
  // (catastrophically early crash with no body) so callers can fall back to
  // the small pill. Built with inline styles: it must look right even if the
  // stylesheet never loaded and it must appear above every app layer
  // (touch pad 250, modals 350).
  function showCrashOverlay(report, autoOpened) {
    try {
      if (!document.body) return false;
      const old = document.getElementById('crash-overlay');
      if (old) old.remove();
      const isManual = report.kind === 'Manual report';
      const root = document.createElement('div');
      root.id = 'crash-overlay';
      root.style.cssText = [
        'position:fixed', 'inset:0', 'z-index:400', 'display:flex',
        'align-items:center', 'justify-content:center', 'padding:18px',
        'background:rgba(0,0,0,.85)', 'font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif',
      ].join(';');
      const panel = document.createElement('div');
      panel.style.cssText = [
        'width:min(430px,96vw)', 'max-height:94vh', 'overflow-y:auto',
        'padding:20px', 'border:1px solid #b3552f', 'border-radius:12px',
        'background:#1e1e22', 'color:#ddd', 'box-shadow:0 12px 44px rgba(0,0,0,.72)',
      ].join(';');
      const h = document.createElement('h2');
      h.textContent = isManual ? 'Report an issue' : 'Something went wrong';
      h.style.cssText = 'margin:0 0 8px;font-size:1.2rem;color:#ffb18a;';
      const msg = document.createElement('p');
      msg.textContent = report.message.slice(0, 300);
      msg.style.cssText = 'margin:0 0 8px;font:12px/1.45 ui-monospace,Consolas,monospace;word-break:break-word;color:#ddd;';
      const sub = document.createElement('p');
      sub.textContent = autoOpened
        ? 'A prefilled GitHub issue (game, system, and log details attached) should have opened in a new tab. If it did not, use the button below.'
        : `The game, system, and log details were collected automatically — no dev tools needed. One tap opens a prefilled GitHub issue${isManual ? '' : ' about this error'} that you can edit before submitting.`;
      sub.style.cssText = 'margin:0 0 14px;color:#aaa;font-size:.84rem;';
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;gap:8px;flex-wrap:wrap;';
      const mkBtn = (label, primary, fn) => {
        const b = document.createElement('button');
        b.textContent = label;
        b.style.cssText = [
          'padding:10px 14px', 'border-radius:8px', 'font-weight:600', 'cursor:pointer',
          primary ? 'border:1px solid #24c8db;background:#24c8db;color:#06262a'
                 : 'border:1px solid #3a3a40;background:transparent;color:#ddd',
        ].join(';');
        b.addEventListener('click', fn);
        return b;
      };
      row.appendChild(mkBtn('Report on GitHub', true, () => {
        // Called from a real user gesture: popup blockers allow this everywhere,
        // including iOS Safari and Android Chrome.
        try { window.open(issueUrl(report), '_blank', 'noopener'); } catch (_) {}
      }));
      const copyBtn = mkBtn('Copy report', false, async () => {
        try { await navigator.clipboard.writeText(report.body); copyBtn.textContent = 'Copied \u2713'; } catch (_) { copyBtn.textContent = 'Copy failed'; }
      });
      row.appendChild(copyBtn);
      row.appendChild(mkBtn(isManual ? 'Close' : 'Keep playing', false, dismiss));
      const tiny = document.createElement('p');
      tiny.textContent = 'Filed to Spuds0588/mgba-splitscreen. Thank you for testing the beta!';
      tiny.style.cssText = 'margin:12px 0 0;color:#777;font-size:.74rem;';
      panel.appendChild(h); panel.appendChild(msg); panel.appendChild(sub);
      panel.appendChild(row); panel.appendChild(tiny);
      root.appendChild(panel);
      function dismiss() {
        root.remove();
        document.removeEventListener('keydown', onKey, true);
        // The report stays in localStorage; the small pill keeps the option
        // alive for later without shouting over the game.
        showReopenPill();
      }
      function onKey(e) { if (e.key === 'Escape') { e.stopPropagation(); dismiss(); } }
      document.addEventListener('keydown', onKey, true);
      document.body.appendChild(root);
      return true;
    } catch (_) {
      return false;
    }
  }

  function reportError(kind, message, err) {
    if (query('crashreport') === 'off') return; // opt-out for A/B testing
    if (reportCount >= MAX_REPORTS_PER_SESSION) {
      console.warn('crash-report: report cap reached; logging locally only');
      return;
    }
    const fingerprint = `${kind}|${message}|${err && err.stack ? String(err.stack).split('\n')[1] : ''}`;
    const now = Date.now();
    if (fingerprint === lastFingerprint && now - lastFingerprintAt < 120000) return;
    lastFingerprint = fingerprint;
    lastFingerprintAt = now;
    reportCount++;
    try { fileIssue(collect(kind, message, err)); } catch (e) {
      console.warn('crash-report: failed to file issue:', e);
    }
  }

  // Small always-available affordance: if the crash tab was blocked, one
  // click re-opens the exact issue that would have been filed.
  function showReopenPill() {
    try {
      let pill = document.getElementById('crash-report-pill');
      if (!pill) {
        pill = document.createElement('button');
        pill.id = 'crash-report-pill';
        pill.type = 'button';
        pill.textContent = 'Crash report ready — click to open the GitHub issue';
        pill.style.cssText = [
          'position:fixed', 'left:12px', 'bottom:12px', 'z-index:9999',
          'padding:8px 12px', 'border-radius:8px', 'border:1px solid #b3552f',
          'background:#2a150d', 'color:#ffb18a', 'font:600 12px/1.2 system-ui,sans-serif',
          'cursor:pointer', 'box-shadow:0 6px 18px rgba(0,0,0,.45)',
        ].join(';');
        pill.addEventListener('click', () => {
          let u = lastIssueUrl;
          if (!u) { try { u = localStorage.getItem('mgs:lastCrashIssueUrl'); } catch (_) {} }
          if (u) window.open(u, '_blank', 'noopener');
          pill.remove();
        });
        document.body ? document.body.appendChild(pill)
          : document.addEventListener('DOMContentLoaded', () => document.body.appendChild(pill), { once: true });
      }
    } catch (_) { /* never fatal */ }
  }

  // ---- Global listeners -------------------------------------------------------

  // capture phase: even a stopped propagation should reach us
  window.addEventListener('error', (event) => {
    // Cross-origin scripts report as "Script error." with no usable detail;
    // WASM aborts arrive through onAbort instead. Neither is worth an issue.
    if (event.message === 'Script error.' || event.message === 'ResizeObserver loop limit exceeded') return;
    const message = event.message || 'Unknown error';
    reportError('Uncaught error', message, event.error);
  }, true);

  window.addEventListener('unhandledrejection', (event) => {
    const reason = event.reason;
    if (reason && (reason.name === 'AbortError')) return; // user-cancelled pickers
    reportError('Unhandled promise rejection', String((reason && reason.message) || reason || '(no reason)'), reason);
  }, true);

  // The WASM runtime calls this on abort (OOM, assertion, unhandled trap).
  window.mgbaCrash = {
    report: reportError,
    sessionLogTail: logTail,
    issueUrl: () => lastIssueUrl,
    // Voluntary "Report an Issue…" (pause menu / Help menu). MUST be called
    // from a click handler so the window.open counts as a user gesture on
    // mobile. Collects the same automatic details as a crash, taken at the
    // moment of the click — exactly what we want for live problems like
    // "the guest audio is garbled".
    manualReport() {
      const report = collect('Manual report', 'Filed by the user from the in-app menu (no crash)', null);
      const url = issueUrl(report);
      try {
        lastReport = report;
        lastIssueUrl = url;
        localStorage.setItem('mgs:lastCrashReport', JSON.stringify(report));
        localStorage.setItem('mgs:lastCrashIssueUrl', url);
      } catch (_) {}
      let opened = false;
      try { opened = !!window.open(url, '_blank', 'noopener'); } catch (_) {}
      if (!opened) showCrashOverlay(report, false);
      return { status: opened ? 'opened' : 'blocked', url };
    },
    // Called by main.js when the emscripten module aborts.
    reportEngineAbort(reason) {
      const bannerShown = (() => {
        try { const el = document.getElementById('hosted-note'); return el && !el.hidden; } catch (_) { return false; }
      })();
      reportError('WASM engine abort', `Engine aborted${bannerShown ? ' (engine failure banner shown)' : ''}: ${reason || '(no reason given)'}`);
    },
    lastReport: () => lastReport,
  };

  // The static fallback page announces that it filed (or saw filed) the
  // issue; we only log — dedup happens on the fallback side.
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin) return;
    if (event.data && event.data.type === 'mgs-crash-report-filed') {
      console.info('crash-report: rescue page handled the issue filing');
    }
  });

  // ?crashreport=test files a report without touching the engine.
  if (query('crashreport') === 'test') {
    setTimeout(() => reportError('Test report', 'Manual test crash report (crashreport=test)'), 1500);
  }
})();
