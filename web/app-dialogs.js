'use strict';

function editTags(source, file = null) {
  editedSource = source?.url || null; editedFiles = null; editedSources = null;
  editedFile = file ? { index: file.index, revision: state.snapshot.catalogRevision } : null;
  editedTags = [...((file || source).tags || [])];
  $('tags-title').textContent = file ? 'File tags' : 'Source tags';
  $('tag-source-name').textContent = file ? `${file.item} / ${file.name}` : source.name || source.item;
  openTagsDialog();
}
function tagSelected() {
  const indices = [...state.selected];
  if (!indices.length) return;
  editedSource = null; editedFile = null; editedSources = null; editedFiles = { indices, revision: state.snapshot.catalogRevision }; editedTags = [];
  $('tags-title').textContent = 'Tag selected files';
  $('tag-source-name').textContent = `Adds tags to ${indices.length.toLocaleString()} selected file${indices.length === 1 ? '' : 's'}. Their existing tags are kept.`;
  openTagsDialog();
}
function tagSelectedSources() {
  const chosen = (state.snapshot?.sources || []).filter(source => source.selected);
  if (!chosen.length) return;
  const grouped = chosen.find(source => collectionById(source.collection));
  if (grouped) { fail(`${grouped.name || grouped.item} is in collection ${collectionById(grouped.collection).name}. Edit the collection's tags instead, or deselect it.`); return; }
  const urls = chosen.map(source => source.url);
  editedSource = null; editedFile = null; editedFiles = null; editedSources = urls; editedTags = [];
  $('tags-title').textContent = 'Tag selected sources';
  $('tag-source-name').textContent = `Adds tags to ${urls.length.toLocaleString()} selected source${urls.length === 1 ? '' : 's'}. Their existing tags are kept.`;
  openTagsDialog();
}
function openTagsDialog() {
  $('tag-input').value = ''; $('tags-error').hidden = true;
  const suggestions = [...(state.snapshot?.sources || []).flatMap(entry => entry.tags || []), ...state.files.flatMap(entry => entry.tags || [])];
  tagSuggestions = [...new Set(suggestions)].sort((left, right) => left.localeCompare(right, undefined, { sensitivity: 'base' }));
  renderEditedTags(); $('tags-dialog').showModal(); $('tag-input').focus();
}
// Tappable chips instead of <datalist>, which many phone browsers do not show.
function renderSuggestions(container, all, chosen, query, add) {
  const text = tagKey(query.trim()), taken = new Set(chosen.map(tagKey));
  const matches = all.filter(tag => !taken.has(tagKey(tag)) && (!text || tagKey(tag).includes(text))).slice(0, 60);
  $(container).replaceChildren(...matches.map(tag => {
    const chip = node('button', tag, 'filter-chip suggestion'); chip.type = 'button'; chip.title = `Add tag ${tag}`;
    chip.setAttribute('aria-label', chip.title); chip.prepend(icon('plus')); chip.addEventListener('click', () => add(tag)); return chip;
  }));
  $(container).hidden = !matches.length;
}
function renderEditedTags() {
  $('edited-tags').replaceChildren(...editedTags.map(tag => {
    const chip = node('button', tag, 'filter-chip'); chip.type = 'button'; chip.title = `Remove tag ${tag}`; chip.setAttribute('aria-label', chip.title); chip.append(icon('close'));
    chip.addEventListener('click', () => { editedTags = editedTags.filter(value => value !== tag); renderEditedTags(); }); return chip;
  }));
  renderSuggestions('tag-suggestions', tagSuggestions, editedTags, $('tag-input').value, tag => addTag(tag));
}
function addTag(tag = $('tag-input').value.trim()) {
  if (!tag) return true;
  if (new TextEncoder().encode(tag).length > 128 || /[\x00-\x1f\x7f]/.test(tag) || editedTags.length >= 32) {
    $('tags-error').textContent = 'Use up to 32 tags, each at most 128 bytes, with no control characters.'; $('tags-error').hidden = false; return false;
  }
  if (!editedTags.some(value => tagKey(value) === tagKey(tag))) editedTags.push(tag);
  $('tag-input').value = ''; $('tags-error').hidden = true; renderEditedTags(); return true;
}
let collectionDialog = null, collectionTagSuggestions = [];
const tagModes = {
  join: [['add', 'Add them to the collection'], ['discard', 'Discard them'], ['choose', 'Choose which to add']],
  leave: [['remove', 'Remove them from the sources'], ['copy', 'Keep them on the sources'], ['choose', 'Choose which to keep']]
};
function byName(left, right) { return left.name.localeCompare(right.name, undefined, { numeric: true, sensitivity: 'base' }); }
function sourcesByUrl(urls) { const wanted = new Set(urls); return (state.snapshot?.sources || []).filter(source => wanted.has(source.url)); }
function uniqueTags(tags) { const seen = new Map(); for (const tag of tags) if (!seen.has(tagKey(tag))) seen.set(tagKey(tag), tag); return [...seen.values()]; }
function openCollectionDialog(mode, { id = '', urls = [] } = {}) {
  const collection = collectionById(id);
  collectionDialog = { mode, id, urls, tags: [...(collection?.tags || [])], offered: [] };
  $('collection-error').hidden = true; $('collection-tag-input').value = '';
  $('collection-name').value = mode === 'edit' ? collection.name : '';
  if (mode === 'add') {
    const choices = [...(state.snapshot?.collections || [])].sort(byName);
    $('collection-choice').replaceChildren(...choices.map(entry => new Option(entry.name, entry.id)), new Option('New collection...', ''));
    $('collection-choice').value = choices[0]?.id || '';
  }
  collectionTagSuggestions = uniqueTags([...(state.snapshot?.sources || []).flatMap(source => source.tags || []), ...state.files.flatMap(file => file.tags || [])])
    .sort((left, right) => left.localeCompare(right, undefined, { sensitivity: 'base' }));
  renderCollectionDialog(); $('collection-dialog').showModal();
}
function renderCollectionDialog() {
  const { mode, id, urls } = collectionDialog, adding = mode === 'add', editing = mode === 'edit';
  const collection = collectionById(adding ? $('collection-choice').value : id);
  const members = (state.snapshot?.sources || []).filter(source => collection && source.collection === collection.id);
  const chosen = sourcesByUrl(urls), plural = count => `${count} source${count === 1 ? '' : 's'}`;
  $('collection-title').textContent = adding ? 'Add to collection' : editing ? 'Edit collection' : mode === 'remove' ? 'Remove from collection' : 'Delete collection?';
  $('collection-choice-label').hidden = !adding;
  $('collection-name-label').hidden = !(editing || (adding && !collection));
  $('collection-tags-editor').hidden = !editing; $('delete-collection').hidden = !editing;
  $('save-collection-label').textContent = adding ? 'Add' : editing ? 'Save' : mode === 'remove' ? 'Remove' : 'Delete collection';
  $('save-collection').className = `${mode === 'delete' ? 'danger' : 'primary'} source-op`;
  const moving = adding && chosen.some(source => source.collection && source.collection !== collection?.id);
  $('collection-summary').textContent = adding ? `${plural(chosen.length)} selected.${moving ? ' Sources already in another collection will move.' : ''}`
    : editing ? `${plural(members.length)}. Tags apply to every source in the collection.`
    : mode === 'remove' ? `${chosen.map(source => source.name || source.item).join(', ')} will leave ${collection.name}.`
    : `${collection.name} is deleted. Its ${plural(members.length)} ${members.length === 1 ? 'stays' : 'stay'} in your source list.`;
  collectionDialog.offered = adding ? uniqueTags(chosen.filter(source => !collection || source.collection !== collection.id).flatMap(source => source.tags || []))
    : editing ? [] : [...collection.tags];
  const offered = collectionDialog.offered;
  $('collection-tag-choice').hidden = !offered.length;
  if (offered.length) {
    $('collection-tag-legend').textContent = adding ? `The selected sources have tags: ${offered.join(', ')}` : `The collection's tags: ${offered.join(', ')}`;
    $('collection-tag-modes').replaceChildren(...tagModes[adding ? 'join' : 'leave'].map(([value, label], index) => {
      const choice = node('label', undefined, 'check'), radio = node('input');
      radio.type = 'radio'; radio.name = 'collection-tag-mode'; radio.value = value; radio.checked = index === 0;
      radio.addEventListener('change', () => { $('collection-tag-picks').hidden = value !== 'choose'; updateCollectionWarning(); });
      choice.append(radio, document.createTextNode(label)); return choice;
    }));
    $('collection-tag-picks').replaceChildren(...offered.map(tag => {
      const choice = node('label', undefined, 'check'), box = node('input');
      box.type = 'checkbox'; box.value = tag; box.checked = true; box.addEventListener('change', updateCollectionWarning);
      choice.append(box, document.createTextNode(tag)); return choice;
    }));
    $('collection-tag-picks').hidden = true;
  }
  if (editing) renderCollectionTags();
  updateCollectionWarning();
}
function collectionTagAnswer() {
  const mode = document.querySelector('[name="collection-tag-mode"]:checked')?.value;
  const keep = [...$('collection-tag-picks').querySelectorAll('input:checked')].map(box => box.value);
  return { tags: collectionDialog.offered.length && mode ? mode : collectionDialog.mode === 'add' ? 'add' : 'remove', keep };
}
function keptTags() {
  const { tags, keep } = collectionTagAnswer(), offered = collectionDialog.offered;
  return tags === 'add' || tags === 'copy' ? offered : tags === 'choose' ? offered.filter(tag => keep.includes(tag)) : [];
}
// Tags also pick download folders, so warn when sources lose a tag that a location uses.
function updateCollectionWarning() {
  const { mode, id, urls } = collectionDialog, settings = state.snapshot.settings;
  let dropped = [];
  if (mode === 'add') {
    const collection = collectionById($('collection-choice').value);
    const result = new Set([...(collection?.tags || []), ...keptTags()].map(tagKey));
    dropped = sourcesByUrl(urls).flatMap(source => source.tags || []).filter(tag => !result.has(tagKey(tag)));
  } else if (mode === 'remove' || mode === 'delete') {
    const kept = new Set(keptTags().map(tagKey));
    dropped = (collectionById(id)?.tags || []).filter(tag => !kept.has(tagKey(tag)));
  } else if (mode === 'edit') {
    const kept = new Set(collectionDialog.tags.map(tagKey));
    dropped = (collectionById(id)?.tags || []).filter(tag => !kept.has(tagKey(tag)));
  }
  const lost = new Set(dropped.map(tagKey));
  const affected = settings.locationsSupported ? settings.locations.filter(location => (location.tags || []).some(tag => lost.has(tagKey(tag)))) : [];
  $('collection-warning').hidden = !affected.length;
  $('collection-warning').textContent = affected.length ? `Downloads from these sources will no longer go to ${affected.map(location => location.name).join(', ')}, because they lose the tag${lost.size === 1 ? '' : 's'} ${uniqueTags(dropped).join(', ')}.` : '';
}
function renderCollectionTags() {
  $('collection-tags').replaceChildren(...collectionDialog.tags.map(tag => {
    const chip = node('span', undefined, 'tag'); chip.append(document.createTextNode(tag), iconButton('close', `Remove collection tag: ${tag}`, () => {
      collectionDialog.tags = collectionDialog.tags.filter(value => value !== tag); renderCollectionTags();
    })); return chip;
  }));
  renderSuggestions('collection-tag-suggestions', collectionTagSuggestions, collectionDialog.tags, $('collection-tag-input').value, tag => addCollectionTag(tag));
  updateCollectionWarning();
}
function collectionError(message) { $('collection-error').textContent = message; $('collection-error').hidden = false; }
function addCollectionTag(tag = $('collection-tag-input').value.trim()) {
  if (!tag) return true;
  if (new TextEncoder().encode(tag).length > 128 || /[\x00-\x1f\x7f]/.test(tag) || collectionDialog.tags.length >= 32) {
    collectionError('Use up to 32 tags, each at most 128 bytes, with no control characters.'); return false;
  }
  if (!collectionDialog.tags.some(value => tagKey(value) === tagKey(tag))) collectionDialog.tags.push(tag);
  $('collection-tag-input').value = ''; $('collection-error').hidden = true; renderCollectionTags(); return true;
}
function renderQueue() {
  const queue = state.snapshot?.queue || [];
  const ids = new Set(queue.map(entry => entry.id));
  for (const [id, row] of state.queueNodes) { if (!ids.has(id)) { row.remove(); state.queueNodes.delete(id); } }
  queue.forEach(entry => {
    let row = state.queueNodes.get(entry.id);
    if (!row) {
    row = node('div', undefined, 'queue-row'); row.append(icon('file'));
    const file = node('div', undefined, 'queue-file'); file.append(node('strong', entry.name));
    const meta = node('div', undefined, 'queue-meta'); meta.append(node('span', entry.item), node('span', bytes(entry.size))); file.append(meta);
    if (!android && entry.destination) file.append(pathLabel(entry.destination));
    const progress = node('div', undefined, 'progress'); const fill = node('span');
    progress.append(fill); file.append(progress, node('div', '', 'queue-detail'));
    const settings = iconButton('settings', 'Open S3 settings', () => { $('settings-button').click(); $('access-key').focus(); });
    settings.classList.add('s3-settings'); file.append(settings);
    const retry = iconButton('play', `Retry download ${entry.name}`, () => run('retryQueue', { id: entry.id }));
    retry.classList.add('retry-download'); file.append(retry);
    const status = node('div', undefined, 'queue-status'); status.append(node('span', '', 'badge'), node('span', '', 'numbers'));
    row.append(file, status, iconButton('trash', `Remove download ${entry.name}`, () => run('removeQueue', { id: entry.id })));
    state.queueNodes.set(entry.id, row); $('queue-list').append(row);
    }
    const staged = queueStage(entry);
    row.querySelector('.progress').classList.toggle('stage', staged !== null);
    row.querySelector('.progress span').style.width = `${(staged ?? (entry.size ? entry.received / entry.size : 0)) * 100}%`;
    row.querySelector('.queue-detail').textContent = entry.detail; row.querySelector('.queue-detail').hidden = !entry.detail;
    const settings = row.querySelector('.s3-settings');
    settings.hidden = entry.status !== 'Failed' || !entry.detail.includes('Archive.org denied access:'); settings.disabled = state.pending;
    const retry = row.querySelector('.retry-download');
    retry.hidden = entry.status !== 'Failed'; retry.disabled = queueBusy() || entry.removing;
    const badge = row.querySelector('.badge'); badge.textContent = entry.removing ? 'Removing' : staged !== null ? entry.stage : entry.status;
    badge.className = `badge ${entry.status === 'Failed' ? 'failed' : 'warn'}`;
    row.querySelector('.numbers').textContent = staged !== null
      ? `${Math.floor(staged * 100)}%${entry.stageTotal ? ` · ${bytes(entry.stageDone)} / ${bytes(entry.stageTotal)}` : ''}`
      : `${bytes(entry.received)} / ${bytes(entry.size)}`;
    row.querySelector(':scope > button').disabled = state.pending || entry.removing;
  });
  $('queue-empty').hidden = queue.length !== 0;
  $('queue-summary').textContent = `${queue.length} ${queue.length === 1 ? 'file' : 'files'}`;
}
// Fraction done of the step after the transfer (joining parts, verifying, extracting), or null outside one.
function queueStage(entry) {
  if (!entry.stage || !entry.status.startsWith('Transferring')) return null;
  return entry.stageTotal ? Math.min(1, entry.stageDone / entry.stageTotal) : 0;
}
function sidebarProgress() {
  const queue = state.snapshot.queue;
  let index = queue.findIndex(entry => entry.status.startsWith('Transferring'));
  if (index < 0) index = queue.findIndex(entry => entry.status === 'Paused');
  if (index < 0) index = queue.findIndex(entry => entry.status === 'Queued');
  $('sidebar-progress').hidden = index < 0;
  if (index >= 0) {
    const entry = queue[index], staged = queueStage(entry);
    const percent = Math.min(100, (staged ?? (entry.size ? entry.received / entry.size : 0)) * 100);
    $('sidebar-fill').style.width = `${percent}%`;
    $('sidebar-fill').parentElement.classList.toggle('stage', staged !== null);
    $('sidebar-label').textContent = `${entry.status === 'Paused' ? 'Paused ' : ''}${index + 1}/${queue.length}  ${staged !== null ? `${entry.stage} ` : ''}${Math.floor(percent)}%`;
    $('sidebar-progress').title = entry.name;
  }
}
async function refresh() {
  try {
    const previousRevision = state.snapshot?.catalogRevision;
    state.snapshot = await request('/api/state');
    state.queueMap = new Map(state.snapshot.queue.map(entry => [JSON.stringify([entry.item, entry.name]), entry]));
    if (previousRevision !== undefined && previousRevision !== state.snapshot.catalogRevision) state.selected.clear();
    $('source-count').textContent = state.snapshot.sources.length; $('library-count').textContent = (state.libraryKey ? state.files.length : state.snapshot.count).toLocaleString(); $('queue-count').textContent = state.snapshot.queue.length;
    $('activity').textContent = state.snapshot.activity; $('busy-label').textContent = state.snapshot.busy ? state.snapshot.activity : '';
    const defaultPath = state.snapshot.settings.destination;
    $('destination').textContent = !android && state.snapshot.settings.locationsSupported ? `Default: ${middlePath(defaultPath, 70)}` : defaultPath;
    $('destination').title = defaultPath;
    renderLocations();
    $('connection').textContent = consoleTarget ? 'Console connected' : 'Local service'; $('connection-light').classList.add('online');
    if ($('alert').dataset.connection && !$('alert').hidden) $('alert').hidden = true;
    delete $('alert').dataset.connection;
    const sourceKey = `${state.snapshot.sourceRevision}/${state.snapshot.tagRevision}/${state.snapshot.scanning}`;
    if (state.sourceKey !== sourceKey) { renderSources(); state.sourceKey = sourceKey; }
    const queueKey = JSON.stringify(state.snapshot.queue.map(entry => [entry.id, entry.status]));
    if (state.view === 'library') {
      await loadLibrary();
      if (queueKey !== state.queueKey) renderLibrary();
    }
    state.queueKey = queueKey;
    if (state.view === 'downloads') renderQueue();
    sidebarProgress(); updateControls();
    const notice = state.snapshot.error || state.snapshot.notice;
    // A plain notice (such as "Queue complete") never covers an error that is still on screen.
    const errorShown = !$('alert').hidden && !$('alert').classList.contains('notice');
    if (notice && notice !== state.lastNotice) { if (state.snapshot.error || !errorShown) fail(notice, !!state.snapshot.error); state.lastNotice = notice; }
    const extraction = state.snapshot.extractionError;
    if (extraction && extraction !== state.lastExtraction) { if (state.lastExtraction !== undefined) fail(extraction, true); }
    state.lastExtraction = extraction || '';
    $('total-size').textContent = state.files.length ? bytes(state.files.reduce((sum, file) => sum + file.size, 0)) : '';
    await renderDiagnostics();
    if (hostLogging && !hostReady) {
      await request('/api/client/ready', {});
      hostReady = true;
    }
  } catch (error) {
    if (error.login || $('login-dialog').open) return;
    $('connection').textContent = 'Disconnected'; $('connection-light').classList.remove('online');
    if (hostLogging) window.ArchiveFetchDiagnostics?.failure('refresh', error);
    if (state.snapshot) { state.snapshot.busy = true; state.snapshot.scanning = true; }
    updateControls(); fail(`${consoleTarget ? 'Console' : 'Local service'} unavailable: ${error.message}`); $('alert').dataset.connection = 'lost';
  }
}
function confirmAction(title, message, files = [], option = '', checked = false) {
  $('confirm-title').textContent = title; $('confirm-message').textContent = message;
  $('confirm-files').replaceChildren(...files.map(file => node('li', file)));
  $('confirm-option').hidden = !option; $('confirm-option-label').textContent = option; $('confirm-option-input').checked = checked;
  $('confirm-dialog').showModal();
  return new Promise(resolve => { confirmation = resolve; });
}
function answerConfirmation(accepted) { $('confirm-dialog').close(); confirmation?.(accepted); confirmation = null; }
function staleCopy(file) { return file.downloadState === 'Missing' || file.downloadState === 'Location unavailable'; }
function staleCopies(status) { return state.files.flatMap(file => (file.downloadCopies || []).filter(copy => copy.status === status).map(copy => copy.path)); }
async function deleteDownload(file) {
  // Offered only when this is the file's last copy and its source is gone, so the entry would linger.
  const option = file.sourceRemoved && (file.downloadCopies?.length || 0) <= 1 ? 'Also remove it from the Library (its source was removed)' : '';
  const extractedTo = file.downloadCopies?.find(copy => copy.path === file.downloadPath)?.extracted;
  const folderNote = extractedTo ? ' The extracted folder stays.' : '';
  const confirmed = file.downloadState === 'Extracted'
    ? await confirmAction('Remove download record?', `The archive was deleted after extracting. Only the record is removed.${folderNote}`, [file.downloadPath, extractedTo], option, true)
    : file.downloaded
    ? await confirmAction('Delete downloaded file?', `The file is removed from storage. It stays in the library and can be downloaded again.${folderNote}`, [file.downloadPath], option, true)
    : await confirmAction('Remove download record?', file.downloadState === 'Missing'
      ? 'The file is no longer in this folder. Only the record is removed.'
      : 'This location is not available (for example, an unplugged drive). Only the record is removed; nothing on the drive is changed.', [file.downloadPath], option, true);
  if (!confirmed) return;
  const removeEntry = !!option && $('confirm-option-input').checked;
  try { await command('deleteDownload', { index: file.index, revision: state.snapshot.catalogRevision, path: file.downloadPath, removeEntry }); await loadLibrary(true); }
  catch (error) { fail(error.message); }
}
const cleanModes = [
  ['orphans', 'Clear files without sources', 'Removes files whose source was deleted and that were never downloaded. Downloaded files stay, including ones on an unplugged drive.'],
  ['undownloaded', 'Clear undownloaded files', 'Removes files that were never downloaded or whose download is missing. Files on an unplugged drive, changed, queued or partly downloaded files stay.'],
  ['all', 'Clear all', 'Removes the entries only. Downloaded files stay on disk and show as downloaded again after a scan.'],
  ['delete', 'Clear and delete everything', 'Deletes the downloaded files from storage, then removes the entries. Copies on an unplugged drive and changed files are left alone. Sources, collections and settings stay.']
];
let cleanPreview = null;
function cleanError(message) { $('clean-error').textContent = message; $('clean-error').hidden = false; }
function cleanScope() { return $('clean-scope-selected').checked ? { indices: [...state.selected], revision: state.snapshot.catalogRevision } : {}; }
function cleanMode() { return document.querySelector('[name="clean-mode"]:checked')?.value; }
async function openClean() {
  const selected = state.selected.size;
  $('clean-error').hidden = true; $('clean-confirm').value = '';
  $('clean-scope-selected').disabled = !selected; $('clean-scope-selected').checked = !!selected; $('clean-scope-all').checked = !selected;
  $('clean-scope-selected-label').textContent = `Selected files (${selected.toLocaleString()})`;
  $('clean-scope-all-label').textContent = `Whole library (${state.files.length.toLocaleString()})`;
  $('clean-dialog').showModal();
  await loadCleanPreview();
}
async function loadCleanPreview() {
  cleanPreview = null; $('clean-error').hidden = true; $('clean-submit').disabled = true;
  $('clean-options').replaceChildren(node('p', 'Counting files...', 'setting-note')); $('clean-note').textContent = '';
  $('clean-confirm-label').hidden = true; $('clean-kept').hidden = true;
  try { cleanPreview = await request('/api/command', { action: 'cleanLibrary', preview: true, ...cleanScope() }); renderCleanOptions(); }
  catch (error) { $('clean-options').replaceChildren(); cleanError(error.message); }
}
function renderCleanOptions() {
  const preview = cleanPreview, total = preview.total.toLocaleString();
  let first = null;
  $('clean-options').replaceChildren(...cleanModes.map(([mode, title, description]) => {
    const count = preview[mode === 'delete' ? 'all' : mode];
    const unavailable = mode === 'delete' && (!preview.delete.supported || preview.downloading);
    const choice = node('label', undefined, 'check clean-option'), radio = node('input');
    radio.type = 'radio'; radio.name = 'clean-mode'; radio.value = mode; radio.disabled = !count || unavailable;
    if (!radio.disabled && !first) first = radio;
    const text = node('span');
    const extra = mode === 'delete' && preview.delete.files ? `, deletes ${preview.delete.files.toLocaleString()} file${preview.delete.files === 1 ? '' : 's'} (${bytes(preview.delete.bytes)})` : '';
    const why = mode === 'delete' && preview.downloading ? ' Wait for downloads to finish first.' : mode === 'delete' && !preview.delete.supported ? ' Not available on this device.' : '';
    text.append(node('strong', title), node('span', `Removes ${count.toLocaleString()} of ${total}${extra}.${why}`, 'secondary'), node('span', description, 'secondary'));
    radio.addEventListener('change', updateCleanChoice);
    choice.append(radio, text); return choice;
  }));
  if (first) first.checked = true;
  updateCleanChoice();
}
function updateCleanChoice() {
  const mode = cleanMode(), preview = cleanPreview, deleting = mode === 'delete' && preview.delete.files > 0;
  const notes = [];
  if (preview.queued) notes.push(`${preview.queued.toLocaleString()} queued file${preview.queued === 1 ? ' is' : 's are'} kept.`);
  if (mode === 'all' || mode === 'undownloaded') notes.push('Scanning a source again adds its files back.');
  if (!mode) notes.push('Nothing to clean here.');
  $('clean-note').textContent = notes.join(' ');
  $('clean-confirm-label').hidden = !deleting;
  $('clean-kept').hidden = !(mode === 'delete' && preview.delete.keptCount);
  $('clean-kept-title').textContent = `Left on disk (unplugged drive or changed file): ${preview.delete.keptCount.toLocaleString()}`;
  $('clean-kept-list').replaceChildren(...preview.delete.kept.map(path => node('li', path)));
  $('clean-submit-label').textContent = deleting ? 'Delete and remove' : 'Remove';
  $('clean-submit').disabled = !mode;
}
function fileKey(file) { return `${file.item}\n${file.name}`; }
async function submitClean(event) {
  event.preventDefault();
  const mode = cleanMode();
  if (!mode) return;
  if (mode === 'delete' && cleanPreview.delete.files && $('clean-confirm').value !== 'DELETE') { cleanError('Type DELETE to confirm.'); return; }
  const scope = cleanScope();
  const kept = scope.indices ? new Set(state.files.filter(file => state.selected.has(file.index)).map(fileKey)) : null;
  try {
    const result = await command('cleanLibrary', { mode, confirm: $('clean-confirm').value, ...scope });
    if (!result?.ok) return;
    $('clean-dialog').close();
    await loadLibrary(true);
    // Files that were selected and are still in the library stay selected.
    if (kept) { state.files.forEach(file => { if (kept.has(fileKey(file))) state.selected.add(file.index); }); renderLibrary(); }
    const deleted = result.deleted ? `, deleted ${result.deleted.toLocaleString()} (${bytes(result.bytes)})` : '';
    fail(`Removed ${result.removed.toLocaleString()} file${result.removed === 1 ? '' : 's'} from the Library${deleted}`, false);
  } catch (error) { cleanError(error.message); }
}
function renderLocations() {
  const settings = state.snapshot.settings;
  const supported = !android && !!settings.locationsSupported;
  $('library-location-control').hidden = !supported; $('download-locations').hidden = !supported;
  $('legacy-download-folder').hidden = supported; $('destination-input').required = !supported;
  if (!supported) return;
  const key = JSON.stringify([settings.locations, settings.defaultLocation, settings.lastLocation]);
  if (locationsKey === key) return;
  locationsKey = key;
  const select = $('library-location');
  select.replaceChildren(new Option('Auto', 'auto'), new Option('Default', 'fallback'));
  for (const location of settings.locations) {
    if (location.id === 'default' && settings.defaultLocation === 'default') continue;
    const option = new Option(location.id === 'default' ? 'Default (built-in)' : location.name, location.id);
    option.title = location.path; select.add(option);
  }
  select.add(new Option('Add location...', 'add'));
  select.value = settings.lastLocation || 'auto';
  if (!select.value) select.value = 'fallback';
  const rows = settings.locations.map(location => {
    const row = node('div', undefined, 'location-row');
    const radio = node('input', undefined, 'location-default'); radio.type = 'radio'; radio.name = 'default-location';
    radio.value = location.id; radio.checked = location.id === settings.defaultLocation;
    radio.title = `Default location: ${location.name}`; radio.setAttribute('aria-label', radio.title);
    radio.addEventListener('change', async () => {
      const result = await run('defaultLocation', { id: location.id });
      if (!result?.ok) { locationsKey = ''; renderLocations(); }
    });
    const info = node('div', undefined, 'location-info');
    info.append(node('strong', location.name), pathLabel(location.path));
    const tags = node('div', undefined, 'tag-list');
    tags.append(...(location.tags || []).map(tag => node('span', tag, 'tag'))); info.append(tags);
    if (location.extract && settings.extractSupported) info.append(node('span', 'Extracts archives', 'secondary'));
    row.append(radio, info, iconButton('settings', `Edit location: ${location.name}`, () => openLocation(location)));
    if (location.id !== 'default') row.append(iconButton('trash', `Remove location: ${location.name}`, async () => {
      if (await confirmAction('Remove location?', location.name, [location.path])) await run('removeLocation', { id: location.id });
    }));
    return row;
  });
  $('location-list').replaceChildren(...rows);
}
function locationError(message) { $('location-error').textContent = message; $('location-error').hidden = false; }
function renderLocationTags() {
  $('location-tags').replaceChildren(...locationTags.map(tag => {
    const chip = node('span', undefined, 'tag'); chip.append(document.createTextNode(tag), iconButton('close', `Remove location tag: ${tag}`, () => {
      locationTags = locationTags.filter(value => value !== tag); renderLocationTags();
    })); return chip;
  }));
  renderSuggestions('location-tag-suggestions', locationTagSuggestions, locationTags, $('location-tag-input').value, tag => addLocationTag(tag));
}
function addLocationTag(tag = $('location-tag-input').value.trim()) {
  if (!tag) return true;
  const owner = state.snapshot.settings.locations.find(location => location.id !== editedLocation && (location.tags || []).some(value => tagKey(value) === tagKey(tag)));
  if (owner) { locationError(`Tag '${tag}' already belongs to ${owner.name}`); return false; }
  if (!locationTags.some(value => tagKey(value) === tagKey(tag))) {
    if (locationTags.length >= 32) { locationError('Use at most 32 tags'); return false; }
    locationTags.push(tag);
  }
  $('location-tag-input').value = ''; $('location-error').hidden = true; renderLocationTags(); return true;
}
function openLocation(location = null, fromLibrary = false) {
  editedLocation = location?.id || null; selectAddedLocation = fromLibrary;
  locationTags = [...(location?.tags || [])];
  $('location-title').textContent = location ? 'Edit location' : 'Add location';
  $('location-name').value = location?.name || ''; $('location-name').readOnly = location?.id === 'default';
  $('location-path').value = location?.path || ''; $('location-tag-input').value = ''; $('location-error').hidden = true;
  $('location-extract-label').hidden = !state.snapshot.settings.extractSupported; $('location-extract').checked = !!location?.extract;
  const suggestions = new Map();
  const tags = [...state.files.flatMap(file => [...(file.tags || []), ...(file.sourceTags || [])]), ...state.snapshot.sources.flatMap(source => source.tags || [])];
  const owned = new Set(state.snapshot.settings.locations.filter(entry => entry.id !== editedLocation).flatMap(entry => (entry.tags || []).map(tagKey)));
  for (const tag of tags) if (!owned.has(tagKey(tag))) suggestions.set(tagKey(tag), tag);
  locationTagSuggestions = [...suggestions.values()].sort((left, right) => left.localeCompare(right, undefined, { sensitivity: 'base' }));
  renderLocationTags(); $('location-dialog').showModal();
}
function chooseRouting(conflicts) {
  $('routing-options').replaceChildren(...conflicts.map(conflict => {
    const label = node('label', conflict.name);
    const select = node('select'); select.dataset.index = conflict.index; select.required = true;
    select.add(new Option('Choose location', ''));
    for (const location of conflict.locations) {
      const option = new Option(`${location.name} (${middlePath(location.path, 36)})`, location.id); option.title = location.path; select.add(option);
    }
    label.append(select); return label;
  }));
  $('routing-dialog').showModal();
  return new Promise(resolve => { routingAnswer = resolve; });
}
function diagnosticEntries(current) {
  return [['activity', current.activity], ['notice', current.notice], ['error', current.error],
    ...current.errors.map((error, index) => [`catalog:${index}`, error]),
    ...current.sources.filter(source => source.error).map(source => [`source:${source.url}`, `${source.name}: ${source.error}`]),
    ...current.queue.filter(entry => entry.detail).map(entry => [`queue:${entry.id}`, `${entry.name}: ${entry.detail}`])].filter(([, text]) => text);
}
async function renderDiagnostics() {
  if (!$('debug-dialog').open || !state.snapshot) return;
  const revision = ++diagnosticsRender;
  let log = '';
  try { log = (await request('/api/diagnostics')).log; } catch { log = 'Network log unavailable'; }
  if (revision !== diagnosticsRender) return;
  const entries = diagnosticEntries(state.snapshot), current = new Map(entries);
  for (const [key, text] of clearedDiagnostics) { if (current.get(key) !== text) clearedDiagnostics.delete(key); }
  $('diagnostics').textContent = [...entries.filter(([key, text]) => clearedDiagnostics.get(key) !== text).map(([, text]) => text), log].filter(Boolean).join('\n\n') || 'No diagnostics';
}
