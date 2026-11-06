'use strict';

const $ = id => document.getElementById(id);
const initialView = ['sources', 'library', 'downloads'].includes(location.hash.slice(1)) ? location.hash.slice(1) : 'library';
const state = { token: '', snapshot: null, files: [], selected: new Set(), view: initialView, page: 0,
  sort: 'name', descending: false, tagScope: 'all', pending: false, libraryKey: '', sourceKey: '', queueKey: '', lastNotice: '', queueNodes: new Map(), queueMap: new Map() };
const pageSize = 100;
const filters = { sources: { source: new Set(), tags: new Set() },
  library: { source: new Set(), extension: new Set(), tags: new Set(), access: new Set(), favorite: new Set(), status: new Set() } };
const sourceSort = { sort: 'name', descending: false };
const filterLabels = { source: 'Sources', extension: 'Extensions', tags: 'Tags', access: 'Access', favorite: 'Favorites', status: 'Status' };
let filterContext = 'library';
let columnContext = 'library';
const hiddenColumns = { sources: new Set(), library: new Set() };
try {
  const saved = JSON.parse(localStorage.getItem('archivefetch-hidden-columns') || '{}');
  for (const context of Object.keys(hiddenColumns)) {
    if (Array.isArray(saved[context])) hiddenColumns[context] = new Set(saved[context].filter(value => typeof value === 'string' && value !== 'name'));
  }
} catch {}
const columnWidths = { sources: {}, library: {} };
try {
  const saved = JSON.parse(localStorage.getItem('archivefetch-column-widths') || '{}');
  for (const context of Object.keys(columnWidths)) {
    for (const [key, width] of Object.entries(saved[context] || {})) {
      if (Number.isFinite(width) && width >= 24 && width <= 1600) columnWidths[context][key] = width;
    }
  }
} catch {}
let editedSource = null;
let editedFile = null;
let editedFiles = null;
let editedSources = null;
let tagSuggestions = [];
let locationTagSuggestions = [];
let editedTags = [];
let editedLocation = null;
let locationTags = [];
let selectAddedLocation = false;
let locationsKey = '';
let routingAnswer = null;
let libraryChecked = 0;
let visible = [];
let confirmation = null;
let clearedDiagnostics = new Map();
let diagnosticsRender = 0;
let hostLogging = null;
let hostReady = false;
let folderBrowser = false;
let remoteAccess = false;
let logFile = false;
const s3KeysUrl = 'https://archive.org/login?referer=%2Faccount%2Fs3.php';
const folderState = { input: null, path: '', parent: '' };
const nativeApp = !!window.ArchiveTransport;
// The saved console this app is controlling through its own backend; '' is this device.
let consoleTarget = '';
try { consoleTarget = localStorage.getItem('archivefetch-target') || ''; } catch {}
// Android storage rules (picker, sharing, app-managed paths) only apply to this device.
const android = nativeApp && !consoleTarget;
function savedTextSize() { try { return localStorage.getItem('archivefetch-text-size') || 'auto'; } catch { return 'auto'; } }
const mediaOriginals = new Map();
// Media queries ignore CSS zoom, so their widths are scaled to switch layouts at the zoomed width.
function scaleMediaQueries(scale) {
  for (const sheet of document.styleSheets) {
    let rules;
    try { rules = sheet.cssRules; } catch { continue; }
    for (const rule of rules) {
      if (!(rule instanceof CSSMediaRule)) continue;
      if (!mediaOriginals.has(rule)) mediaOriginals.set(rule, rule.media.mediaText);
      const scaled = mediaOriginals.get(rule).replace(/(\d+(?:\.\d+)?)px/g, (_, value) => `${Math.round(value * scale)}px`);
      if (rule.media.mediaText !== scaled) rule.media.mediaText = scaled;
    }
  }
}
// The console browser shows a desktop-width page on a TV, so its default is larger.
function applyTextSize() {
  const saved = savedTextSize();
  const scale = saved === 'auto' ? (/PlayStation/i.test(navigator.userAgent) ? 1.5 : 1) : Number(saved) / 100 || 1;
  document.documentElement.style.zoom = scale === 1 ? '' : String(scale);
  scaleMediaQueries(scale);
  // Zoom scales px lengths, so --vh (used instead of vh) is the viewport height divided by the zoom.
  document.documentElement.style.setProperty('--vh', `${innerHeight / scale / 100}px`);
}
window.addEventListener('resize', applyTextSize); applyTextSize();
// The console browser has no hover, but its pointer reports one, so hover-only controls stay faintly visible there.
document.documentElement.classList.toggle('console-browser', /PlayStation/i.test(navigator.userAgent));
let sidebarCollapsed = false;
let openCollections = new Set();
try { openCollections = new Set(JSON.parse(localStorage.getItem('archivefetch-open-collections') || '[]')); } catch {}
function saveOpenCollections() { try { localStorage.setItem('archivefetch-open-collections', JSON.stringify([...openCollections])); } catch {} }
try { sidebarCollapsed = localStorage.getItem('archivefetch-sidebar') === 'collapsed'; } catch {}
function applySidebar() {
  const label = sidebarCollapsed ? 'Expand menu' : 'Collapse menu', button = $('collapse-sidebar');
  document.documentElement.classList.toggle('sidebar-collapsed', sidebarCollapsed);
  button.title = label; button.setAttribute('aria-label', label); button.setAttribute('aria-expanded', String(!sidebarCollapsed));
}
$('collapse-sidebar').addEventListener('click', () => {
  sidebarCollapsed = !sidebarCollapsed;
  try { localStorage.setItem('archivefetch-sidebar', sidebarCollapsed ? 'collapsed' : 'expanded'); } catch {}
  applySidebar();
});
applySidebar();
const nativeRequests = new Map();
let nativeSequence = 0;
if (nativeApp) {
  window.ArchiveTransport.onmessage = event => {
    const response = JSON.parse(event.data);
    const pending = nativeRequests.get(response.id);
    if (!pending) return;
    nativeRequests.delete(response.id); clearTimeout(pending.timer); pending.resolve(response.result);
  };
}
function nativeRequest(path, data) {
  return new Promise((resolve, reject) => {
    const id = ++nativeSequence;
    const timer = setTimeout(() => { nativeRequests.delete(id); reject(new Error('Android request timed out')); }, ['chooseCatalog', 'chooseDirectory'].includes(data?.action) ? 300000 : 30000);
    nativeRequests.set(id, { resolve, timer });
    window.ArchiveTransport.postMessage(JSON.stringify({ id, path, data, token: state.token }));
  });
}

function node(tag, text, className) {
  const element = document.createElement(tag);
  if (text !== undefined) element.textContent = text;
  if (className) element.className = className;
  return element;
}
function icon(name) { const element = node('i', undefined, `icon ${name}`); element.setAttribute('aria-hidden', 'true'); return element; }
function iconButton(name, label, callback, disabled = false) {
  const button = node('button', undefined, 'icon-button');
  button.type = 'button'; button.title = label; button.setAttribute('aria-label', label);
  button.append(icon(name)); button.disabled = disabled; button.addEventListener('click', callback); return button;
}
function bytes(value) {
  if (!value) return '0 B';
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  const unit = Math.min(4, Math.floor(Math.log(value) / Math.log(1024)));
  return `${(value / 1024 ** unit).toLocaleString(undefined, { maximumFractionDigits: unit ? 2 : 0 })} ${units[unit]}`;
}
function fail(message, error = true) {
  delete $('alert').dataset.connection; $('alert-action').hidden = true; $('alert-action').onclick = null;
  $('alert-text').textContent = message; $('alert').classList.toggle('notice', !error); $('alert').hidden = false;
}
function notify(message, label, action) {
  fail(message, false);
  $('alert-action').textContent = label; $('alert-action').hidden = false;
  $('alert-action').onclick = () => { $('alert').hidden = true; $('alert-action').hidden = true; action(); };
}
function loginRequired(result) {
  showLogin();
  const error = new Error(result.error || 'Sign in required'); error.login = true; return error;
}
// While a console is chosen, its requests go through this app's backend, which holds the console's session.
// The page keeps only this device's token; the list of consoles and the diagnostics stay on this device.
const localOnly = /^\/api\/(consoles|relay|diagnostics|clipboard|client\/|logging)/;
function request(path, data) {
  if (!consoleTarget || localOnly.test(path) || (path !== '/session' && !path.startsWith('/api/'))) return localRequest(path, data);
  return localRequest('/api/relay', { console: consoleTarget, path, ...(data === undefined ? {} : { body: data }) });
}
async function localRequest(path, data, retried = false) {
  if (nativeApp) {
    const result = await nativeRequest(path, data);
    if (result.login) throw loginRequired(result);
    if (result.error && (path !== '/api/state' || !('busy' in result))) throw new Error(result.error);
    return result;
  }
  const response = await fetch(path, { method: data === undefined ? 'GET' : 'POST', credentials: 'same-origin', cache: 'no-store',
    headers: { 'X-Archive-Session': state.token, ...(data === undefined ? {} : { 'Content-Type': 'application/json' }) },
    body: data === undefined ? undefined : JSON.stringify(data), signal: AbortSignal.timeout(30000) });
  // The desktop service rejects a stale token with an empty 403 body.
  const result = await response.json().catch(() => ({}));
  if (response.status === 401 && result.login) throw loginRequired(result);
  // A restarted service issues a new token; renew once instead of failing until the page is reloaded.
  if (response.status === 403 && !retried && path !== '/session') { await renewSession(); return localRequest(path, data, true); }
  if (!response.ok) throw new Error(result.error || `${consoleTarget ? 'The console' : 'Local service'} returned ${response.status}`);
  return result;
}
function showVersion(session) { if (session.version) $('app-version').textContent = `v${session.version}`; }
async function renewSession() {
  const session = await localRequest('/session');
  if (session.token !== state.token) { state.libraryKey = ''; state.sourceKey = ''; state.queueKey = ''; state.selected.clear(); }
  state.token = session.token; showVersion(session);
}
async function command(action, fields = {}) {
  if (state.pending) return null;
  state.pending = true; updateControls();
  try { return await request('/api/command', { action, ...fields }); }
  finally { state.pending = false; await refresh(); updateControls(); }
}
function run(action, fields = {}) { return command(action, fields).catch(error => fail(error.message)); }
function busy() { return state.pending || !state.snapshot || state.snapshot.busy; }
function sourceBusy() { return state.pending || !state.snapshot || state.snapshot.scanning; }
// Queueing works during downloads and scans.
function queueBusy() { return state.pending || !state.snapshot || !state.snapshot.loaded; }
function updateControls() {
  document.querySelectorAll('.idle-only').forEach(button => { button.disabled = busy(); });
  document.querySelectorAll('.source-op').forEach(button => { button.disabled = sourceBusy(); });
  $('enqueue').disabled = queueBusy() || !state.selected.size;
  $('library-location').disabled = queueBusy();
  $('tag-selected').disabled = busy() || !state.selected.size;
  $('tag-sources').disabled = sourceBusy() || !state.snapshot?.sources.some(source => source.selected);
  $('add-to-collection').disabled = $('tag-sources').disabled;
  $('share-downloads').hidden = !android;
  $('share-downloads').disabled = busy() || !state.selected.size || !state.files.filter(file => state.selected.has(file.index)).every(file => file.downloaded);
  $('scan').disabled = sourceBusy() || !state.snapshot?.sources.some(source => source.selected);
  $('start').disabled = queueBusy() || state.snapshot.downloading || !state.snapshot.queue.length;
  $('delete-sources').disabled = sourceBusy() || !state.snapshot?.sources.length;
  $('all-sources').disabled = sourceBusy(); $('select-page').disabled = queueBusy();
  $('stop').hidden = !state.snapshot?.busy; $('stop').disabled = state.pending;
  $('stop-label').textContent = state.snapshot?.scanning ? 'Stop scan' : 'Stop';
  $('save-settings').disabled = busy();
  $('save-tags').disabled = editedFile || editedFiles ? busy() : sourceBusy();
  document.querySelectorAll('#location-list input, #location-list button').forEach(control => { control.disabled = busy() || !state.snapshot?.settings.locationsWritable; });
  document.querySelectorAll('#settings-form .browse, #forget').forEach(button => { button.disabled = busy(); });
  // Import reads a file on this device, so it is not offered while a console is chosen.
  $('import').hidden = !!consoleTarget; $('import').disabled = busy(); $('export').disabled = busy() || !state.snapshot?.loaded;
  $('settings-button').disabled = state.pending;
  $('clear-verified').disabled = state.pending || !state.snapshot?.queue.some(entry => entry.status === 'Verified');
  document.querySelectorAll('#library-body input').forEach(control => { control.disabled = queueBusy(); });
  document.querySelectorAll('#library-body button').forEach(control => { control.disabled = busy(); });
  document.querySelectorAll('#sources-body input, #sources-body button').forEach(control => { control.disabled = sourceBusy(); });
  $('selection-count').textContent = state.selected.size ? `${state.selected.size.toLocaleString()} selected` : '';
}
function view(name) {
  state.view = name;
  history.replaceState(null, '', `#${name}`);
  document.querySelectorAll('.view').forEach(section => { section.hidden = section.id !== `${name}-view`; });
  document.querySelectorAll('[data-view]').forEach(button => {
    button.classList.toggle('active', button.dataset.view === name);
    if (button.dataset.view === name) button.setAttribute('aria-current', 'page'); else button.removeAttribute('aria-current');
  });
  $('page-title').textContent = name[0].toUpperCase() + name.slice(1);
  if (name === 'library') loadLibrary(true).catch(error => fail(error.message));
  if (name === 'downloads') renderQueue();
}
