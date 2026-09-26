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
    lines.push(`${report.kind} on the web build (reported automatically by the in-app beta reporter).`);
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
    url.searchParams.set('title', `[auto] ${report.kind}: ${report.message.slice(0, 160)}`);
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
    showReopenPill();
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
