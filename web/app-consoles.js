// Saved consoles: the sidebar switcher, the "Choose what to control" dialog and their status checks.
let consolesSupported = false;
let consoleList = [];
let consolePolling = false;
let editingConsole = '';
let targetsKey = '';
let payloadAvailable = false;
let appVersion = '';
const consoleStatusText = { online: 'Online', signin: 'Needs sign-in', offline: 'Offline', refused: 'Refused the connection', checking: 'Checking' };
function consoleState(entry) { return entry.status === 'online' && entry.older ? 'older' : entry.status; }
function installText(install) {
  if (install.stage === 'sending') return `Sending ArchiveFetch ${install.version}: ${Math.floor(install.sent * 100 / Math.max(1, install.total))}%`;
  if (install.stage === 'starting') return `Sent; waiting for ArchiveFetch ${install.version} to start`;
  if (install.stage === 'syncing') return `ArchiveFetch ${install.version} started; updating its Payload Manager copy: ${Math.floor(install.sent * 100 / Math.max(1, install.total))}%`;
  if (install.stage === 'done') return install.warning || `Updated to ArchiveFetch ${install.version}${install.payloadManager === 'updated' ? ', including its Payload Manager copy' : ''}`;
  return `Update failed: ${install.error}`;
}
function consoleDetail(entry) {
  const parts = [entry.port === 8890 ? entry.address : `${entry.address}:${entry.port}`,
    entry.status === 'refused' && entry.detail ? entry.detail : consoleStatusText[entry.status] || entry.status];
  if (entry.version) parts.push(entry.older ? `ArchiveFetch ${entry.version}, older than this app` : entry.newer ? `ArchiveFetch ${entry.version}, newer than this app` : `ArchiveFetch ${entry.version}`);
  if (entry.paired) parts.push('Paired');
  if (entry.install) parts.push(installText(entry.install));
  return parts.join(' · ');
}
// Offered for an older ArchiveFetch, and for a console that does not answer (it may only be running its loader).
function installLabel(entry) {
  if (!payloadAvailable || entry.install?.running) return '';
  if (entry.install?.warning && !entry.newer) return 'Retry update';
  if (entry.older) return 'Update ArchiveFetch';
  return entry.status === 'offline' || entry.status === 'refused' ? 'Install ArchiveFetch' : '';
}
async function installConsole(entry) {
  const message = entry.selfUpdate && entry.status !== 'offline' && entry.status !== 'refused'
    ? `This app sends its ArchiveFetch payload to ArchiveFetch on ${entry.name}, which starts it with the console's own ELF loader. The loader must be running on the console, but its port does not need to be reachable from here.`
    : `This app sends its ArchiveFetch payload to ${entry.address} on port ${entry.loader}, the console's ELF loader. Start the loader on the console first; a running ArchiveFetch hands over to the new one.`;
  if (!await confirmAction(`${installLabel(entry)} on ${entry.name}?`, `${message} After it starts, this app also replaces its existing ArchiveFetch copy in Payload Manager, under the same name and without changing the boot list. If that fails, the running update stays in place and a warning is shown.`)) return;
  try {
    let result = await localRequest('/api/consoles', { action: 'install', id: entry.id });
    payloadAvailable = !!result.payload; consoleList = result.consoles; renderTargets();
    // Kept up while the dialog is closed too, so the banner shows the progress.
    while (consoleList.find(saved => saved.id === entry.id)?.install?.running) {
      await new Promise(resolve => setTimeout(resolve, 1000));
      result = await localRequest('/api/consoles', {});
      consoleList = result.consoles; renderTargets();
    }
  } catch (error) { if ($('targets-dialog').open) consoleError(error.message); else fail(error.message); }
}
// One banner for a console whose ArchiveFetch differs from this app: the console being controlled, or on This device the first saved one.
function bannerKey(entry) { return `archivefetch-banner:${entry.id}:${entry.version}:${appVersion}`; }
function bannerConsole() {
  const differs = entry => entry.install?.running || ((entry.install?.warning || (entry.version && (entry.older || entry.newer))) &&
    (() => { try { return localStorage.getItem(bannerKey(entry)) !== 'dismissed'; } catch { return true; } })());
  if (consoleTarget) return consoleList.find(entry => entry.id === consoleTarget && differs(entry)) || null;
  return consoleList.find(differs) || null;
}
function renderUpdateBanner() {
  const entry = bannerConsole();
  $('update-banner').hidden = !entry;
  if (!entry) return;
  const install = entry.install;
  const newer = !install?.running && entry.newer;
  let text, action = '';
  if (install?.running) text = `${entry.name}: ${installText(install)}`;
  else if (install?.warning) {
    text = `${entry.name}: ${install.warning}`;
    if (payloadAvailable && !entry.newer) action = 'Retry update';
  }
  // This app only carries its own version, so it never puts an older ArchiveFetch on the console.
  else if (newer) text = `${entry.name} runs ArchiveFetch ${entry.version}, which is newer than this app (${appVersion}). Install ArchiveFetch ${entry.version} on this device to match.`;
  else {
    text = `${entry.name} runs ArchiveFetch ${entry.version}; this app has ${appVersion}.`;
    if (install?.stage === 'failed') text += ` ${installText(install)}`;
    if (payloadAvailable) action = 'Update ArchiveFetch';
  }
  $('update-banner').className = newer ? 'newer' : '';
  $('update-banner-text').textContent = text;
  $('update-banner-action').textContent = action; $('update-banner-action').hidden = !action;
  $('update-banner-dismiss').hidden = !!install?.running;
}
$('update-banner-action').addEventListener('click', () => { const entry = bannerConsole(); if (entry) installConsole(entry); });
$('update-banner-dismiss').addEventListener('click', () => {
  const entry = bannerConsole();
  if (entry) try { localStorage.setItem(bannerKey(entry), 'dismissed'); } catch {}
  renderUpdateBanner();
});
function chooseTarget(id) {
  try { if (id) localStorage.setItem('archivefetch-target', id); else localStorage.removeItem('archivefetch-target'); } catch {}
  location.reload();
}
function showTarget() {
  const saved = consoleList.find(entry => entry.id === consoleTarget);
  $('target-name').textContent = saved ? saved.name : 'This device';
  $('target-icon').className = `icon ${saved ? 'gamepad' : 'desktop'}`;
  $('target-dot').className = `target-dot ${saved ? consoleState(saved) : 'online'}`;
  const label = saved ? `Controlling ${saved.name} (${consoleDetail(saved)}). Choose what to control` : 'Controlling this device. Choose what to control';
  $('target-button').title = label; $('target-button').setAttribute('aria-label', label);
  if (saved) $('login-title').textContent = `Sign in to ${saved.name}`;
  $('merge-button').hidden = !consolesSupported || !consoleList.length;
  renderUpdateBanner();
}
function renderTargets() {
  showTarget();
  if (!$('targets-dialog').open) return;
  const rows = [{ id: '', name: 'This device', detail: 'This app\'s own Sources, Library and Downloads', state: 'online' },
    ...consoleList.map(entry => ({ id: entry.id, name: entry.name, detail: consoleDetail(entry), state: consoleState(entry), install: installLabel(entry) }))];
  // Rebuilt only when something changed, so focus stays put while statuses are polled.
  const key = JSON.stringify([consoleTarget, rows]);
  if (key === targetsKey) return;
  targetsKey = key;
  $('target-list').replaceChildren(...rows.map(row => {
    const item = node('li', undefined, `target-row${row.id === consoleTarget ? ' current' : ''}`);
    const choose = node('button', undefined, 'target-choose'); choose.type = 'button';
    const dot = node('i', undefined, `target-dot ${row.state}`); dot.setAttribute('aria-hidden', 'true');
    const text = node('span', undefined, 'target-text'); text.append(node('strong', row.name), node('small', row.detail));
    choose.append(icon(row.id ? 'gamepad' : 'desktop'), dot, text);
    if (row.id === consoleTarget) choose.setAttribute('aria-current', 'true');
    choose.addEventListener('click', () => { if (row.id === consoleTarget) $('targets-dialog').close(); else chooseTarget(row.id); });
    item.append(choose);
    const entry = consoleList.find(saved => saved.id === row.id);
    if (entry && row.install) item.append(iconButton('upload', `${row.install} on ${row.name}`, () => installConsole(entry)));
    if (entry) item.append(iconButton('pen', `Edit ${row.name}`, () => editConsole(entry)), iconButton('trash', `Remove ${row.name}`, () => removeConsole(entry)));
    return item;
  }));
}
async function loadConsoles(refresh = false) {
  const result = await localRequest('/api/consoles', { refresh });
  payloadAvailable = !!result.payload; appVersion = result.appVersion || appVersion;
  consoleList = result.consoles; renderTargets(); renderScan(result.scan);
}
let scanKey = '';
// Found consoles fill the form so they can be named before saving.
function renderScan(scan) {
  if (!scan || !$('targets-dialog').open) return;
  const found = scan.found || [];
  $('console-scan').disabled = !!scan.running;
  $('scan-status').textContent = scan.running ? `Checking ${scan.checked} of ${scan.total} addresses${scan.network ? ` on ${scan.network}` : ''}`
    : scan.total === undefined ? '' : found.length ? `Found ${found.length} console${found.length === 1 ? '' : 's'} running ArchiveFetch` : 'No consoles running ArchiveFetch were found';
  const key = JSON.stringify(found);
  if (key === scanKey) return;
  scanKey = key;
  $('scan-list').replaceChildren(...found.map(entry => {
    const item = node('li', undefined, 'target-row');
    const state = entry.status === 'online' && entry.older ? 'older' : entry.status;
    const dot = node('i', undefined, `target-dot ${state}`); dot.setAttribute('aria-hidden', 'true');
    const text = node('span', undefined, 'target-text target-choose');
    text.append(node('strong', entry.address), node('small', `${consoleStatusText[entry.status] || entry.status} \u00b7 ArchiveFetch ${entry.version}${entry.older ? ', older than this app' : entry.newer ? ', newer than this app' : ''}`));
    const add = node('button', entry.saved ? 'Saved' : 'Add'); add.type = 'button'; add.disabled = !!entry.saved;
    add.addEventListener('click', () => {
      resetConsoleForm();
      $('console-name').value = 'PS5'; $('console-address').value = entry.address; $('console-port').value = String(entry.port);
      $('console-name').focus(); $('console-name').select();
    });
    item.append(dot, text, add);
    return item;
  }));
}
$('console-scan').addEventListener('click', async () => {
  consoleError('');
  try {
    let scan = (await localRequest('/api/consoles', { action: 'scan' })).scan;
    renderScan(scan);
    while (scan.running && $('targets-dialog').open) {
      await new Promise(resolve => setTimeout(resolve, 1000));
      const result = await localRequest('/api/consoles', {});
      consoleList = result.consoles; renderTargets(); scan = result.scan; renderScan(scan);
    }
  } catch (error) { consoleError(error.message); }
});
function pollConsoles() {
  if (consolePolling) return;
  consolePolling = true;
  setTimeout(async function poll() {
    try { await loadConsoles(); } catch {}
    setTimeout(poll, $('targets-dialog').open ? 2000 : 15000);
  }, 15000);
}
// Returns false when the page is reloading onto this device because the chosen console is gone.
async function startConsoles(local) {
  consolesSupported = !!local.consoles;
  $('target-button').hidden = !consolesSupported;
  $('login-switch').hidden = !consoleTarget;
  if (!consolesSupported) {
    if (!consoleTarget) return true;
    chooseTarget(''); return false;
  }
  await loadConsoles();
  if (consoleTarget && !consoleList.some(entry => entry.id === consoleTarget)) { chooseTarget(''); return false; }
  if (consoleTarget) $('activity').textContent = 'Connecting to the console';
  pollConsoles();
  return true;
}
function consoleError(message) { $('console-error').textContent = message; $('console-error').hidden = !message; }
function resetConsoleForm() {
  editingConsole = ''; $('console-form').reset(); $('console-port').value = '8890'; $('console-loader').value = '9021';
  $('console-form-title').textContent = 'Add a console'; $('console-save').lastChild.textContent = 'Add console';
  $('console-cancel').hidden = true; consoleError('');
}
function editConsole(entry) {
  editingConsole = entry.id;
  $('console-name').value = entry.name; $('console-address').value = entry.address; $('console-port').value = String(entry.port); $('console-loader').value = String(entry.loader || 9021);
  $('console-form-title').textContent = `Edit ${entry.name}`; $('console-save').lastChild.textContent = 'Save console';
  $('console-cancel').hidden = false; consoleError(''); $('console-name').focus();
}
async function removeConsole(entry) {
  if (!await confirmAction(`Remove ${entry.name}?`, 'It is removed from this list only; nothing changes on the console.')) return;
  try {
    consoleList = (await localRequest('/api/consoles', { action: 'remove', id: entry.id })).consoles;
    if (entry.id === consoleTarget) { chooseTarget(''); return; }
    if (editingConsole === entry.id) resetConsoleForm();
    renderTargets();
  } catch (error) { consoleError(error.message); }
}
$('target-button').addEventListener('click', () => {
  resetConsoleForm(); targetsKey = ''; scanKey = '';
  $('targets-dialog').showModal(); renderTargets();
  loadConsoles(true).catch(error => consoleError(error.message));
});
$('console-cancel').addEventListener('click', resetConsoleForm);
$('console-form').addEventListener('submit', async event => {
  event.preventDefault();
  const edited = editingConsole;
  try {
    const result = await localRequest('/api/consoles', { action: edited ? 'edit' : 'add', ...(edited ? { id: edited } : {}),
      name: $('console-name').value, address: $('console-address').value, port: Number($('console-port').value), loader: Number($('console-loader').value) });
    consoleList = result.consoles;
    // A changed address or port for the console in use needs a fresh connection.
    if (edited && edited === consoleTarget) { chooseTarget(edited); return; }
    resetConsoleForm(); renderTargets();
  } catch (error) { consoleError(error.message); }
});
$('login-switch').addEventListener('click', () => chooseTarget(''));

// Merge: the page reads both sides, previews what is new and each tag difference, then sends only additions,
// split into requests below the 1 MiB request limit of the console and the app.
let mergePlan = null;
const mergeChunkLimit = 800000;
function mergeSide(target) {
  return target === 'local' ? data => localRequest('/api/command', data) : data => localRequest('/api/relay', { console: target, path: '/api/command', body: data });
}
function missingTags(from = [], into = []) {
  const have = new Set(into.map(tag => tag.toLowerCase()));
  return from.filter(tag => !have.has(tag.toLowerCase()));
}
function mergeError(message) { $('merge-error').textContent = message; $('merge-error').hidden = !message; }
function openMerge() {
  $('merge-console').replaceChildren(...consoleList.map(entry => new Option(entry.name, entry.id)));
  if (consoleTarget) $('merge-console').value = consoleTarget;
  mergePlan = null; $('merge-apply').disabled = true; $('merge-preview').replaceChildren(); mergeError('');
  $('merge-dialog').showModal();
}
function buildMergePlan(from, into, options) {
  const fileKey = file => `${file.item}\n${file.name}`;
  const plan = { sources: [], collections: [], sourceTags: [], files: [], fileTags: [], favorites: [], conflicts: [] };
  const intoSources = new Map(into.sources.map(source => [source.item, source]));
  const targetItems = new Set(into.sources.map(source => source.item));
  if (options.sources) {
    const groups = new Map(from.collections.map(group => [group.name.toLowerCase(), group]));
    const intoGroups = new Set(into.collections.map(group => group.name.toLowerCase()));
    for (const source of from.sources) {
      const existing = intoSources.get(source.item);
      if (!existing) {
        plan.sources.push({ url: source.url, name: source.name, tags: source.tags, collection: source.collection });
        targetItems.add(source.item);
        const group = source.collection.toLowerCase();
        if (group && !intoGroups.has(group) && !plan.collections.some(entry => entry.name.toLowerCase() === group))
          plan.collections.push(groups.get(group) || { name: source.collection, tags: [] });
        continue;
      }
      // Collection members carry their collection's tags, so they are left as they are.
      if (existing.collection) continue;
      const add = missingTags(source.tags, existing.tags);
      if (!add.length) continue;
      const entry = { url: existing.url, tags: add };
      if (!existing.tags.length) plan.sourceTags.push(entry);
      else plan.conflicts.push({ kind: 'sourceTags', label: existing.name || source.item, have: existing.tags, add, entry, choice: 'add' });
    }
  }
  const intoFiles = new Set(into.files.map(fileKey)), present = new Set(intoFiles);
  // Files only follow sources that exist (or are being added) on the receiving side.
  if (options.catalog)
    for (const file of from.files) if (targetItems.has(file.item) && !intoFiles.has(fileKey(file))) { plan.files.push(file); present.add(fileKey(file)); }
  if (options.tags) {
    const intoTags = new Map(into.fileTags.map(entry => [fileKey(entry), entry.tags]));
    for (const tagged of from.fileTags) {
      if (!present.has(fileKey(tagged))) continue;
      const have = intoTags.get(fileKey(tagged)) || [];
      const add = missingTags(tagged.tags, have);
      if (!add.length) continue;
      const entry = { item: tagged.item, name: tagged.name, tags: add };
      if (!have.length) plan.fileTags.push(entry);
      else plan.conflicts.push({ kind: 'fileTags', label: tagged.name, have, add, entry, choice: 'add' });
    }
  }
  if (options.favorites) {
    const intoFavorites = new Set(into.favorites.map(fileKey));
    for (const file of from.favorites) if (present.has(fileKey(file)) && !intoFavorites.has(fileKey(file))) plan.favorites.push(file);
  }
  return plan;
}
function renderMergePreview() {
  const plan = mergePlan, list = node('ul', undefined, 'merge-summary');
  const line = (count, text) => { if (count) list.append(node('li', `${count.toLocaleString()} ${text}`)); };
  line(plan.sources.length, `new source${plan.sources.length === 1 ? '' : 's'}${plan.sources.length ? ': ' + plan.sources.slice(0, 8).map(source => source.name).join(', ') + (plan.sources.length > 8 ? ', …' : '') : ''}`);
  line(plan.collections.length, `new collection${plan.collections.length === 1 ? '' : 's'}`);
  line(plan.files.length, 'files added to the Library');
  line(plan.sourceTags.length + plan.fileTags.length, 'sources or files gain tags');
  line(plan.favorites.length, 'favorites');
  const content = [node('h3', `Merging into ${plan.label}`), list];
  if (plan.conflicts.length) {
    const header = node('div', undefined, 'merge-conflicts-header');
    header.append(node('strong', `Tag differences (${plan.conflicts.length.toLocaleString()})`));
    for (const [label, choice] of [['Add all', 'add'], ['Keep all', 'keep']]) {
      const button = node('button', label); button.type = 'button';
      button.addEventListener('click', () => { plan.conflicts.forEach(conflict => { conflict.choice = choice; }); renderMergePreview(); });
      header.append(button);
    }
    const rows = node('ul', undefined, 'merge-conflicts');
    for (const conflict of plan.conflicts.slice(0, 300)) {
      const row = node('li'), text = node('span', undefined, 'merge-conflict-text');
      text.append(node('strong', conflict.label), node('small', `Has ${conflict.have.join(', ')} · adds ${conflict.add.join(', ')}`));
      const choice = node('select'); choice.setAttribute('aria-label', `Tags for ${conflict.label}`);
      choice.append(new Option('Add tags', 'add'), new Option('Keep as is', 'keep')); choice.value = conflict.choice;
      choice.addEventListener('change', () => { conflict.choice = choice.value; });
      row.append(text, choice); rows.append(row);
    }
    if (plan.conflicts.length > 300) rows.append(node('li', `${(plan.conflicts.length - 300).toLocaleString()} more follow Add all or Keep all`, 'folder-empty'));
    content.push(header, rows);
  }
  const empty = !plan.sources.length && !plan.files.length && !plan.sourceTags.length && !plan.fileTags.length && !plan.favorites.length && !plan.conflicts.length;
  if (empty) content.push(node('p', 'Nothing to merge: the other side already has everything chosen.', 'setting-note'));
  $('merge-preview').replaceChildren(...content);
  $('merge-apply').disabled = empty;
}
async function previewMerge() {
  mergeError(''); mergePlan = null; $('merge-apply').disabled = true;
  const consoleId = $('merge-console').value;
  if (!consoleId) { mergeError('Add a console first'); return; }
  const toConsole = document.querySelector('[name="merge-direction"]:checked').value === 'to';
  const selected = document.querySelector('[name="merge-scope"]:checked').value === 'selected';
  const options = { sources: $('merge-sources').checked, catalog: $('merge-catalog').checked, tags: $('merge-tags').checked, favorites: $('merge-favorites').checked };
  const name = consoleList.find(entry => entry.id === consoleId)?.name || 'the console';
  $('merge-preview-button').disabled = true; $('merge-preview').textContent = 'Reading both sides';
  try {
    const [from, into] = await Promise.all([mergeSide(toConsole ? 'local' : consoleId)({ action: 'mergeExport', selected }),
      mergeSide(toConsole ? consoleId : 'local')({ action: 'mergeExport' })]);
    mergePlan = buildMergePlan(from, into, options);
    mergePlan.target = toConsole ? consoleId : 'local';
    mergePlan.label = toConsole ? name : 'this device';
    renderMergePreview();
  } catch (error) { $('merge-preview').replaceChildren(); mergeError(error.message); }
  finally { $('merge-preview-button').disabled = false; }
}
function mergeChunks(plan) {
  const sourceTags = [...plan.sourceTags], fileTags = [...plan.fileTags];
  for (const conflict of plan.conflicts) if (conflict.choice === 'add') (conflict.kind === 'sourceTags' ? sourceTags : fileTags).push(conflict.entry);
  const first = { action: 'mergeImport', sources: plan.sources, collections: plan.collections, sourceTags };
  const chunks = [first];
  let current = first, size = JSON.stringify(first).length;
  for (const [key, entries] of [['files', plan.files], ['fileTags', fileTags], ['favorites', plan.favorites]]) {
    for (const entry of entries) {
      const length = JSON.stringify(entry).length + 1;
      if (size + length > mergeChunkLimit) { current = { action: 'mergeImport' }; chunks.push(current); size = 40; }
      (current[key] ||= []).push(entry); size += length;
    }
  }
  return chunks;
}
async function applyMerge(event) {
  event.preventDefault();
  if (!mergePlan) return;
  const send = mergeSide(mergePlan.target), chunks = mergeChunks(mergePlan), total = { sources: 0, files: 0, tags: 0, favorites: 0 };
  $('merge-apply').disabled = true; $('merge-preview-button').disabled = true; mergeError('');
  try {
    for (const [index, chunk] of chunks.entries()) {
      $('merge-preview').textContent = `Merging (${index + 1} of ${chunks.length})`;
      const result = await send(chunk);
      for (const key of Object.keys(total)) total[key] += result[key] || 0;
    }
    $('merge-dialog').close();
    fail(`Merged into ${mergePlan.label}: ${total.sources} sources, ${total.files.toLocaleString()} files, ${total.tags.toLocaleString()} tag changes, ${total.favorites} favorites.`, false);
    state.libraryKey = ''; state.sourceKey = '';
    await refresh();
  } catch (error) {
    // Each part only adds, so retrying after a failure is safe.
    mergeError(`${error.message}. Parts already merged stay; preview again to continue.`);
  } finally { $('merge-preview-button').disabled = false; mergePlan = null; }
}
$('merge-button').addEventListener('click', openMerge);
$('merge-preview-button').addEventListener('click', previewMerge);
$('merge-form').addEventListener('submit', applyMerge);
