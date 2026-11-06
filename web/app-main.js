'use strict';


document.querySelectorAll('[data-view]').forEach(button => button.addEventListener('click', () => view(button.dataset.view)));
$('go-sources').addEventListener('click', () => view('sources')); $('go-library').addEventListener('click', () => view('library'));
$('dismiss-alert').addEventListener('click', () => { $('alert').hidden = true; });
$('stop').addEventListener('click', () => run('stop', state.snapshot?.scanning ? { lane: 'scan' } : {})); $('start').addEventListener('click', () => run('start')); $('scan').addEventListener('click', () => run('scan'));
$('clear-verified').addEventListener('click', () => run('clearVerified'));
$('add-source').addEventListener('submit', async event => {
  event.preventDefault(); const result = await run('addSource', { url: $('source-url').value.trim(), related: $('related').checked });
  if (result?.ok) $('source-url').value = '';
});
$('all-sources').addEventListener('change', event => run('selectSources', { selected: event.target.checked, urls: filteredSources().map(source => source.url) }));
$('delete-sources').addEventListener('click', async () => { if (await confirmAction('Delete all sources?', 'The cached library and downloads will remain.')) await run('clearSources'); });
$('search').addEventListener('input', () => applyFilters('library'));
$('source-search').addEventListener('input', () => applyFilters('sources'));
document.querySelectorAll('[data-filters]').forEach(button => {
  const context = button.dataset.filters;
  button.addEventListener('click', () => openFilters(context));
  const clear = node('button', undefined); clear.id = `${context}-clear-filters`; clear.hidden = true;
  clear.append(icon('clear'), document.createTextNode('Clear filters'));
  clear.addEventListener('click', () => clearFilters(context)); button.after(clear);
});
document.addEventListener('pointerdown', event => closeFilterDropdowns(event.target.closest('.multi-select')));
$('filters-dialog').addEventListener('close', () => closeFilterDropdowns());
$('filter-sort').addEventListener('change', event => { (filterContext === 'library' ? state : sourceSort).sort = event.target.value; applyFilters(filterContext); });
document.querySelectorAll('[name="sort-direction"]').forEach(radio => radio.addEventListener('change', () => {
  (filterContext === 'library' ? state : sourceSort).descending = radio.value === 'descending'; applyFilters(filterContext);
}));
$('reset-filters').addEventListener('click', () => clearFilters(filterContext));
$('add-tag').addEventListener('click', () => { addTag(); $('tag-input').focus(); });
$('tag-input').addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); addTag(); } });
$('tag-input').addEventListener('input', renderEditedTags);
$('location-tag-input').addEventListener('input', renderLocationTags);
$('tag-selected').addEventListener('click', tagSelected);
$('tag-sources').addEventListener('click', tagSelectedSources);
$('add-to-collection').addEventListener('click', () => {
  const urls = (state.snapshot?.sources || []).filter(source => source.selected).map(source => source.url);
  if (urls.length) openCollectionDialog('add', { urls });
});
$('collection-choice').addEventListener('change', renderCollectionDialog);
$('delete-collection').addEventListener('click', () => { collectionDialog.mode = 'delete'; renderCollectionDialog(); });
$('add-collection-tag').addEventListener('click', () => { addCollectionTag(); $('collection-tag-input').focus(); });
$('collection-tag-input').addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); addCollectionTag(); } });
$('collection-tag-input').addEventListener('input', renderCollectionTags);
$('collection-form').addEventListener('submit', async event => {
  event.preventDefault();
  const { mode, id, urls } = collectionDialog, name = $('collection-name').value.trim(), target = $('collection-choice').value;
  if ((mode === 'edit' || (mode === 'add' && !target)) && !name) { collectionError('Name the collection.'); return; }
  if (mode === 'edit' && !addCollectionTag()) return;
  try {
    const result = mode === 'edit' ? await command('collectionEdit', { id, name, tags: collectionDialog.tags })
      : mode === 'add' ? await command('collectionAdd', { urls, ...(target ? { id: target } : { name }), ...collectionTagAnswer() })
      : mode === 'remove' ? await command('collectionRemove', { urls, ...collectionTagAnswer() })
      : await command('collectionDelete', { id, ...collectionTagAnswer() });
    if (!result?.ok) return;
    if (result.id) { openCollections.add(result.id); saveOpenCollections(); renderSources(); }
    $('collection-dialog').close();
  } catch (error) { collectionError(error.message); }
});
$('tags-form').addEventListener('submit', async event => {
  event.preventDefault(); if (!addTag()) return;
  if ((editedFiles || editedSources) && !editedTags.length) { $('tags-error').textContent = 'Add at least one tag.'; $('tags-error').hidden = false; return; }
  try {
    const result = editedSources ? await command('addSourceTags', { urls: editedSources, tags: editedTags })
      : editedFiles ? await command('addFileTags', { ...editedFiles, tags: editedTags })
      : editedFile ? await command('fileTags', { ...editedFile, tags: editedTags }) : await command('sourceTags', { url: editedSource, tags: editedTags });
    if (result?.ok) { $('tags-dialog').close(); if (editedFile || editedFiles) await loadLibrary(true); }
  }
  catch (error) { $('tags-error').textContent = error.message; $('tags-error').hidden = false; }
});
$('select-page').addEventListener('change', event => { visible.forEach(file => { if (event.target.checked) state.selected.add(file.index); else state.selected.delete(file.index); }); renderLibrary(); });
$('previous').addEventListener('click', () => { --state.page; renderLibrary(); }); $('next').addEventListener('click', () => { ++state.page; renderLibrary(); });
$('enqueue').addEventListener('click', async () => {
  const fields = { indices: [...state.selected], revision: state.snapshot.catalogRevision, location: $('library-location').value || 'auto',
    locationRevision: state.snapshot.settings.locationRevision, tagRevision: state.snapshot.tagRevision, sourceRevision: state.snapshot.sourceRevision };
  let result = await run('enqueue', fields);
  if (result?.locationConflicts) {
    const choices = await chooseRouting(result.locationConflicts);
    if (!choices) return;
    fields.locationChoices = choices; result = await run('enqueue', fields);
  }
  if (result?.confirmOverwrite && await confirmAction('Replace existing files?', 'Existing files will only be replaced after the new downloads pass verification.', result.confirmOverwrite)) result = await run('enqueue', { ...fields, overwrite: true });
  if (result?.ok) { state.selected.clear(); renderLibrary(); }
});
$('import').addEventListener('click', async () => { const result = await run('chooseCatalog'); if (result?.path) await run('import', { path: result.path }); });
$('export').addEventListener('click', () => run('export'));
$('clean-library').addEventListener('click', openClean);
document.querySelectorAll('[name="clean-scope"]').forEach(radio => radio.addEventListener('change', loadCleanPreview));
$('clean-form').addEventListener('submit', submitClean);
$('share-downloads').addEventListener('click', () => run('share', { indices: [...state.selected], revision: state.snapshot.catalogRevision }));
$('settings-button').addEventListener('click', () => {
  if (!state.snapshot) return;
  const settings = state.snapshot.settings;
  $('location-preference').value = settings.locationPreference || 'auto';
  $('destination-input').value = settings.destination; $('output-input').value = settings.output;
  $('automatic').checked = settings.automatic; $('remember').checked = settings.remember;
  $('extract-settings').hidden = !settings.extractSupported;
  $('extract-archives').checked = !!settings.extractArchives; $('delete-extracted').checked = !!settings.deleteExtracted;
  $('excluded-extensions').value = settings.excludedExtensions ?? '';
  $('remember-label').textContent = settings.keysProtected === false ? 'Remember keys on this device (saved without encryption)' : 'Remember keys on this device';
  $('network-settings').hidden = !remoteAccess; $('log-settings').hidden = !logFile;
  $('password-new').value = ''; $('password-repeat').value = ''; passwordError('');
  if (remoteAccess) loadRemoteAccess();
  if (logFile) loadLogSize();
  if (hostLogging) $('host-log-level').value = hostLogging.configured;
  $('auth-status').textContent = settings.authenticated ? (settings.remember ? 'Saved keys loaded' : 'Session keys loaded') : 'Not configured';
  $('access-key').placeholder = settings.authenticated ? 'Unchanged' : ''; $('secret-key').placeholder = settings.authenticated ? 'Unchanged' : '';
  $('access-key').value = ''; $('secret-key').value = ''; $('settings-error').hidden = true; $('text-size').value = savedTextSize(); $('settings-dialog').showModal();
  loadLibrary(true).catch(error => fail(error.message));
});
document.querySelectorAll('.close-dialog').forEach(button => button.addEventListener('click', () => button.closest('dialog').close()));
$('settings-dialog').addEventListener('close', () => { $('access-key').value = ''; $('secret-key').value = ''; loadLibrary(true).catch(error => fail(error.message)); });
document.querySelectorAll('.browse').forEach(button => button.addEventListener('click', async () => {
  if (folderBrowser) { await openFolderBrowser(button.dataset.input); return; }
  const result = await run('chooseDirectory'); if (result?.path) $(button.dataset.input).value = result.path;
}));
$('settings-form').addEventListener('submit', async event => {
  event.preventDefault();
  try {
    const result = await command('settings', { destination: $('destination-input').value, output: $('output-input').value,
      ...(!android && state.snapshot.settings.locationsSupported ? { defaultLocation: state.snapshot.settings.defaultLocation, locationPreference: $('location-preference').value } : {}),
      automatic: $('automatic').checked, remember: $('remember').checked,
      ...(state.snapshot.settings.extractSupported ? { extractArchives: $('extract-archives').checked, deleteExtracted: $('delete-extracted').checked } : {}),
      excludedExtensions: $('excluded-extensions').value,
      access: $('access-key').value, secret: $('secret-key').value });
    if (result?.ok) {
      if (hostLogging) hostLogging = await request('/api/logging', { level: $('host-log-level').value });
      $('settings-dialog').close();
    }
  } catch (error) { $('settings-error').textContent = error.message; $('settings-error').hidden = false; }
});
$('forget').addEventListener('click', async () => {
  const result = await run('forget'); if (result?.ok) { $('auth-status').textContent = 'Not configured'; $('remember').checked = false; $('access-key').value = ''; $('secret-key').value = ''; $('access-key').placeholder = ''; $('secret-key').placeholder = ''; }
});
$('get-s3-keys').addEventListener('click', async () => {
  // Hosts without a desktop browser to launch (the console) show the keys page in a new tab, or on the PS5 a QR code to continue on a phone.
  // An app controlling a console opens the page on this device; the keys are then saved on the console.
  if (consoleTarget) {
    try { await localRequest('/api/command', { action: 'openS3Keys' }); }
    catch (error) { $('settings-error').textContent = error.message; $('settings-error').hidden = false; }
  } else if (android || !remoteAccess) {
    try { await command('openS3Keys'); }
    catch (error) { $('settings-error').textContent = error.message; $('settings-error').hidden = false; }
  } else if (/PlayStation/i.test(navigator.userAgent)) {
    $('settings-dialog').close();
    openNetworkDialog('Get your keys on another device: scan this code to open ArchiveFetch on your phone or computer, then open Settings there and press Get S3 keys. Paste both keys into Settings and save.');
  } else window.open(s3KeysUrl, '_blank', 'noopener');
});
function showRemoteAccess(result) {
  $('external-access').checked = result.external;
  $('external-access').disabled = !result.local || (!result.passwordSet && !result.external);
  $('external-error').textContent = !result.local ? 'Only a device on the local network can change this.'
    : !result.passwordSet ? 'Set a password before allowing external connections.' : '';
  $('external-error').hidden = !$('external-error').textContent;
  $('password-status').textContent = result.passwordSet
    ? `A password is set.${result.local ? '' : ' Change it from the console or a device on its local network.'}`
    : `No password is set.${result.external ? ' Devices outside the console\'s network cannot connect until you set one.' : ''}`;
  $('password-fields').hidden = !result.local;
  $('password-save').textContent = result.passwordSet ? 'Change password' : 'Set password';
  $('password-remove').hidden = !result.passwordSet;
  $('sign-out').hidden = !result.signedIn;
}
function passwordError(message) { $('password-error').textContent = message; $('password-error').hidden = !message; }
$('password-save').addEventListener('click', async () => {
  const value = $('password-new').value;
  if (value.length < 8 || value.length > 128) { passwordError('Use a password of 8 to 128 characters.'); return; }
  if (value !== $('password-repeat').value) { passwordError('The two passwords do not match.'); return; }
  try {
    showRemoteAccess(await request('/api/password', { password: value }));
    $('password-new').value = ''; $('password-repeat').value = ''; passwordError('');
    fail('Password saved. Devices outside the network have to sign in again.', false);
  } catch (error) { passwordError(error.message); }
});
$('password-remove').addEventListener('click', async () => {
  if (!await confirmAction('Remove the password?', 'Devices outside the console\'s network will be refused until a new password is set.')) return;
  try { showRemoteAccess(await request('/api/password', { password: '' })); passwordError(''); }
  catch (error) { passwordError(error.message); }
});
$('sign-out').addEventListener('click', async () => {
  try { await request('/api/logout', {}); } catch {}
  // While a console is chosen, state.token is this device's own session and stays valid.
  if (!consoleTarget) state.token = '';
  showLogin();
});
async function loadPairedDevices() {
  $('paired-error').hidden = true;
  let devices;
  // Consoles before 0.7.0-beta.2 have no pairing.
  try { devices = (await request('/api/paired-devices')).devices; } catch { $('paired-section').hidden = true; return; }
  $('paired-section').hidden = false;
  const date = seconds => new Date(seconds * 1000).toLocaleDateString();
  $('paired-list').replaceChildren(...(devices.length ? devices.map(device => {
    const item = node('li', undefined, 'paired-row'), text = node('span', undefined, 'paired-text');
    text.append(node('strong', device.current ? `${device.name} (this app)` : device.name), node('small', `Paired ${date(device.created)} · last used ${date(device.lastSeen)}`));
    item.append(text, iconButton('trash', `Remove ${device.name}`, async () => {
      if (!await confirmAction(`Remove ${device.name}?`, 'It has to sign in with the password again from outside the console\'s network.')) return;
      try { await request('/api/paired-devices/remove', { id: device.id }); await loadPairedDevices(); }
      catch (error) { $('paired-error').textContent = error.message; $('paired-error').hidden = false; }
    }));
    return item;
  }) : [node('li', 'No devices are paired.', 'folder-empty')]));
}
async function loadRemoteAccess() {
  try { showRemoteAccess(await request('/api/remote-access')); }
  catch (error) { $('external-error').textContent = error.message; $('external-error').hidden = false; }
  await loadPairedDevices();
}
$('external-access').addEventListener('change', async event => {
  const external = event.target.checked;
  if (external && !await confirmAction('Allow external connections?', 'Devices that can reach port 8890 from outside, for example through port forwarding, will be able to sign in with your password and then control ArchiveFetch, including deleting downloaded files. ArchiveFetch uses plain HTTP, so on untrusted networks the password can be read in transit.')) {
    event.target.checked = false; return;
  }
  try { showRemoteAccess(await request('/api/remote-access', { external })); }
  catch (error) { event.target.checked = !external; $('external-error').textContent = error.message; $('external-error').hidden = false; }
});
function showLogSize(result) {
  const previous = result.previousBytes ? `, previous log ${bytes(result.previousBytes)}` : '';
  $('log-size').textContent = `run.log is ${bytes(result.bytes)}${previous}. Logs over ${bytes(result.limit)} are moved aside when ArchiveFetch starts.`;
}
async function loadLogSize() {
  try { showLogSize(await request('/api/log')); } catch (error) { $('log-size').textContent = error.message; }
}
$('clear-console-log').addEventListener('click', async () => {
  if (!await confirmAction('Clear the console log?', 'run.log is emptied. The previous log, if any, is kept.')) return;
  try { showLogSize(await request('/api/log/clear', {})); } catch (error) { $('log-size').textContent = error.message; }
});
let logEntries = [], logBytes = 0;
function problemEntry(entry) { return /FAIL|FATAL|DENIED|INVALID|ERROR/.test(entry.event) || Number(entry.details?.status) >= 400; }
function renderLogEntries() {
  const query = $('log-search').value.toLocaleLowerCase().trim();
  const rows = logEntries.filter(entry => !query || entry.search.includes(query));
  $('log-body').replaceChildren(...rows.map(entry => {
    const row = node('tr'); row.classList.toggle('log-problem', problemEntry(entry));
    const time = node('td'); time.append(node('span', entry.time.replace('T', ' ').slice(0, 23)), node('span', entry.version, 'secondary'));
    row.append(time, node('td', entry.event), node('td', entry.text)); return row;
  }));
  $('log-summary').textContent = `Newest first: ${rows.length.toLocaleString()} of the latest ${logEntries.length.toLocaleString()} entries. run.log is ${bytes(logBytes)}.`;
}
async function loadLogEntries() {
  try {
    const result = await request('/api/log/entries?limit=1000');
    logEntries = result.entries.map(entry => {
      const text = Object.entries(entry.details || {}).map(([key, value]) => `${key}=${typeof value === 'string' ? value : JSON.stringify(value)}`).join('  ');
      return { ...entry, text, search: `${entry.event} ${entry.version} ${text}`.toLocaleLowerCase() };
    });
    logBytes = result.bytes; renderLogEntries();
  } catch (error) { $('log-summary').textContent = error.message; }
}
$('open-console-log').addEventListener('click', () => {
  $('log-search').value = ''; $('log-body').replaceChildren(); $('log-summary').textContent = 'Loading...';
  $('log-dialog').showModal(); loadLogEntries();
});
$('refresh-log').addEventListener('click', loadLogEntries);
$('log-search').addEventListener('input', renderLogEntries);
$('add-location').addEventListener('click', () => openLocation());
$('clean-downloads').addEventListener('click', async () => {
  try {
    await loadLibrary(true);
    const missing = staleCopies('Missing'), unavailable = staleCopies('Location unavailable');
    if (!missing.length && !unavailable.length) { fail('No missing downloads to clean up', false); return; }
    const keep = 'Unavailable locations, such as an unplugged drive, are kept unless you tick the box.';
    const message = missing.length ? `These files are no longer in their folders. Their records are removed. ${unavailable.length ? keep : ''}`
      : 'No files are missing from existing folders. The locations below are unavailable; tick the box to remove their records.';
    const files = [...missing, ...unavailable.map(path => `Unavailable: ${path}`)];
    const option = unavailable.length ? `Also remove ${unavailable.length} unavailable record${unavailable.length === 1 ? '' : 's'} (plug drives in first if you still use them)` : '';
    if (!await confirmAction('Clean up missing downloads?', message, files, option)) return;
    const includeUnavailable = $('confirm-option-input').checked;
    if (!missing.length && !includeUnavailable) return;
    const result = await command('cleanDownloads', { unavailable: includeUnavailable });
    if (!result) return;
    await loadLibrary(true);
    fail(`Removed ${result.removed} download record${result.removed === 1 ? '' : 's'}`, false);
  } catch (error) { fail(error.message); }
});
$('library-location').addEventListener('change', async event => {
  if (event.target.value === 'add') {
    event.target.value = state.snapshot.settings.lastLocation || 'auto'; openLocation(null, true);
  } else {
    const result = await run('useLocation', { id: event.target.value });
    if (!result?.ok) { locationsKey = ''; renderLocations(); }
  }
});
$('add-location-tag').addEventListener('click', () => { addLocationTag(); $('location-tag-input').focus(); });
$('location-tag-input').addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); addLocationTag(); } });
$('location-form').addEventListener('submit', async event => {
  event.preventDefault(); if (!addLocationTag()) return;
  try {
    const result = await command(editedLocation ? 'editLocation' : 'addLocation', { id: editedLocation || '', name: $('location-name').value.trim(), path: $('location-path').value.trim(), tags: locationTags, extract: $('location-extract').checked });
    if (result?.ok) {
      if (selectAddedLocation) await command('useLocation', { id: result.id });
      $('location-dialog').close();
    }
  } catch (error) { locationError(error.message); }
});
$('routing-form').addEventListener('submit', event => {
  event.preventDefault();
  const choices = Object.fromEntries([...$('routing-options').querySelectorAll('select')].map(select => [select.dataset.index, select.value]));
  const resolve = routingAnswer; routingAnswer = null; $('routing-dialog').close(); resolve?.(choices);
});
$('routing-dialog').addEventListener('close', () => { routingAnswer?.(null); routingAnswer = null; });
$('confirm-accept').addEventListener('click', () => answerConfirmation(true)); $('confirm-cancel').addEventListener('click', () => answerConfirmation(false));
$('confirm-dialog').addEventListener('cancel', event => { event.preventDefault(); answerConfirmation(false); });
$('reload-interface').addEventListener('click', () => location.reload());
$('debug-button').addEventListener('click', () => { $('debug-dialog').showModal(); renderDiagnostics(); });
$('copy-diagnostics').addEventListener('click', async () => {
  try {
    if (android) await request('/api/clipboard', { text: $('diagnostics').textContent });
    else await navigator.clipboard.writeText($('diagnostics').textContent);
  } catch { fail('Cannot access clipboard'); }
});
$('clear-log').addEventListener('click', async () => {
  const baseline = new Map(diagnosticEntries(state.snapshot)); ++diagnosticsRender;
  try { await request('/api/diagnostics/clear', {}); clearedDiagnostics = baseline; await renderDiagnostics(); }
  catch (error) { fail(error.message); }
});

function renderNetworkCode() {
  const url = $('network-address').value;
  if (!url) return;
  const code = qrcode(0, 'M');
  code.addData(url); code.make();
  const canvas = $('network-qr');
  const count = code.getModuleCount();
  const cell = 7;
  canvas.width = canvas.height = (count + 8) * cell;
  const context = canvas.getContext('2d');
  context.fillStyle = '#fff'; context.fillRect(0, 0, canvas.width, canvas.height);
  context.fillStyle = '#000';
  for (let row = 0; row < count; ++row) {
    for (let column = 0; column < count; ++column) {
      if (code.isDark(row, column)) context.fillRect((column + 4) * cell, (row + 4) * cell, cell, cell);
    }
  }
  canvas.hidden = false;
  $('network-url').value = url; $('network-url').hidden = false;
}
$('network-address').addEventListener('change', renderNetworkCode);
$('text-size').addEventListener('change', () => { try { localStorage.setItem('archivefetch-text-size', $('text-size').value); } catch {} applyTextSize(); });
$('network-url').addEventListener('click', event => event.target.select());
$('network-button').addEventListener('click', () => openNetworkDialog());
async function openNetworkDialog(note = '') {
  $('network-note').textContent = note; $('network-note').hidden = !note;
  $('network-error').hidden = true; $('network-qr').hidden = true; $('network-url').hidden = true;
  $('network-address-label').hidden = true;
  $('network-dialog').showModal();
  try {
    const result = await request('/api/network');
    $('network-address').replaceChildren();
    for (const url of result.urls) $('network-address').append(new Option(url, url));
    if (!result.urls.length) throw new Error('No LAN address is available');
    if (typeof qrcode === 'undefined') {
      await new Promise((resolve, reject) => {
        const script = document.createElement('script');
        script.src = 'assets/qrcode.js'; script.onload = resolve;
        script.onerror = () => { script.remove(); reject(new Error('Cannot load the QR generator')); };
        document.head.append(script);
      });
    }
    $('network-address-label').hidden = result.urls.length < 2;
    renderNetworkCode();
  } catch (error) { $('network-error').textContent = error.message; $('network-error').hidden = false; }
}

async function loadFolder(path) {
  $('folder-error').hidden = true;
  try {
    const result = await request(`/api/folders?path=${encodeURIComponent(path)}`);
    folderState.path = result.path; folderState.parent = result.parent;
    $('folder-current').textContent = result.path || 'Storage';
    $('folder-up').disabled = !result.path; $('folder-choose').disabled = !result.path;
    $('folder-new-name').disabled = $('folder-create').disabled = !result.path || !result.writable;
    const list = $('folder-list'); list.replaceChildren();
    for (const folder of result.folders) {
      const button = node('button', undefined, 'folder-entry'); button.type = 'button';
      button.append(icon('folder'), node('span', folder.free === undefined ? folder.name : `${folder.name} · ${bytes(folder.free)} free`));
      button.addEventListener('click', () => loadFolder(folder.path));
      const item = node('li'); item.append(button); list.append(item);
    }
    if (!result.folders.length) list.append(node('li', result.path ? 'No subfolders' : 'No storage found', 'folder-empty'));
    if (result.truncated) list.append(node('li', 'Only the first 2,000 folders are shown', 'folder-empty'));
    return true;
  } catch (error) { $('folder-error').textContent = error.message; $('folder-error').hidden = false; return false; }
}
async function openFolderBrowser(input) {
  folderState.input = input; $('folder-new-name').value = '';
  $('folder-dialog').showModal();
  if (!await loadFolder($(input).value.trim())) await loadFolder('');
}
$('folder-up').addEventListener('click', () => loadFolder(folderState.parent));
$('folder-choose').addEventListener('click', () => {
  if (!folderState.path) return;
  $(folderState.input).value = folderState.path; $('folder-dialog').close();
});
$('folder-create').addEventListener('click', async () => {
  const name = $('folder-new-name').value.trim();
  if (!name) return;
  try {
    const result = await request('/api/folders', { path: folderState.path, name });
    $('folder-new-name').value = ''; await loadFolder(result.path);
  } catch (error) { $('folder-error').textContent = error.message; $('folder-error').hidden = false; }
});

async function reconnectNativeSession() {
  try {
    const token = (await localRequest('/session')).token;
    if (token !== state.token) {
      state.libraryKey = ''; state.sourceKey = ''; state.queueKey = '';
      state.selected.clear();
    }
    state.token = token;
    await refresh();
  } catch (error) { fail(`Cannot connect: ${error.message}`); }
}
async function connect() {
  const local = await localRequest('/session'); state.token = local.token; showVersion(local);
  hostLogging = local.logging || null;
  $('host-logging-settings').hidden = !hostLogging;
  if (hostLogging) window.ArchiveFetchDiagnostics?.connect(state.token);
  let session = local;
  if (!await startConsoles(local)) return;
  if (consoleTarget) session = await request('/session');
  $('network-button').hidden = !session.network;
  folderBrowser = !!session.folderBrowser;
  remoteAccess = !!session.remoteAccess; logFile = !!session.logFile;
  // The console's run.log replaces the in-app diagnostics there; see Settings > Console log. Apps keep their own.
  $('debug-button').hidden = logFile && !consoleTarget;
  await refresh();
}
function showLogin() {
  if ($('login-dialog').open) return;
  $('login-error').hidden = true; $('login-password').value = '';
  document.querySelectorAll('dialog[open]').forEach(dialog => dialog.close());
  $('login-dialog').showModal(); $('login-password').focus();
}
$('login-dialog').addEventListener('cancel', event => event.preventDefault());
$('login-form').addEventListener('submit', async event => {
  event.preventDefault();
  $('login-submit').disabled = true;
  try {
    await request('/api/login', { password: $('login-password').value });
    $('login-password').value = ''; $('login-dialog').close();
    await connect();
  } catch (error) {
    if (!error.login) { $('login-error').textContent = error.message; $('login-error').hidden = false; }
  } finally { $('login-submit').disabled = false; }
});
async function start() {
  try { await connect(); }
  catch (error) { if (!error.login) { window.ArchiveFetchDiagnostics?.failure('session', error); fail(`Cannot connect: ${error.message}`); } }
  setTimeout(async function poll() { await refresh(); setTimeout(poll, 800); }, 800);
}
if (android) {
  ['destination-input', 'output-input'].forEach(id => { $(id).readOnly = true; });
  document.querySelectorAll('.browse').forEach(button => { button.hidden = button.dataset.input !== 'destination-input'; });
}
view(state.view); updateControls(); start();
