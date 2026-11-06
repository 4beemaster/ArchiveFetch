'use strict';

async function loadLibrary(force = false) {
  if (!state.snapshot) return;
  const key = `${state.snapshot.catalogRevision}/${state.snapshot.sourceRevision}/${state.snapshot.tagRevision}/${state.snapshot.downloadRevision}/${state.snapshot.settings.locationRevision}/${state.snapshot.settings.destination}/${state.snapshot.settings.excludedExtensions}`;
  if (!force && key === state.libraryKey && Date.now() - libraryChecked < 10000) return;
  const result = await request('/api/library');
  if (state.snapshot.catalogRevision !== result.revision || state.snapshot.sourceRevision !== result.sourceRevision || state.snapshot.tagRevision !== result.tagRevision) return;
  const files = result.files;
  state.files = files; state.libraryKey = key; libraryChecked = Date.now();
  $('clean-downloads').hidden = !staleCopies('Missing').length && !staleCopies('Location unavailable').length;
  const valid = new Set(files.map(file => file.index));
  state.selected.forEach(index => { if (!valid.has(index)) state.selected.delete(index); });
  renderLibrary();
}
function queueStatus(file) {
  const queued = state.queueMap.get(JSON.stringify([file.item, file.name]));
  if (queued) return { text: queued.status.startsWith('Transferring') ? 'Downloading' : queued.status, kind: queued.status === 'Failed' ? 'failed' : 'warn', queued: true };
  return { text: (!android && file.downloadState) || (file.downloaded ? 'Downloaded' : 'Not downloaded'), kind: file.downloaded ? 'good' : !android && file.downloadPath ? 'warn' : '', queued: false };
}
function middlePath(path, limit = 52) {
  if (path.length <= limit) return path;
  const start = Math.floor((limit - 1) * .4);
  return `${path.slice(0, start)}\u2026${path.slice(-(limit - start - 1))}`;
}
function pathLabel(path, limit = 52) {
  const label = node('span', middlePath(path, limit), 'download-path');
  label.title = path; label.setAttribute('aria-label', path); return label;
}
function filename(file) { return file.name.slice(file.name.lastIndexOf('/') + 1); }
function extension(file) {
  const name = filename(file), dot = name.lastIndexOf('.');
  return dot > 0 && dot < name.length - 1 ? name.slice(dot).toLowerCase() : '';
}
function hideTypeButton(type) {
  const button = node('button', undefined, 'hide-type'); button.type = 'button';
  button.title = `Hide ${type} files`; button.setAttribute('aria-label', button.title); button.append(icon('eye-slash'));
  button.addEventListener('mousedown', event => event.preventDefault());
  button.addEventListener('click', event => { event.stopPropagation(); hideFileType(type); });
  return button;
}
function extensionCell(type) {
  const cell = node('td', undefined, 'extension-cell');
  cell.append(node('span', type)); if (type) cell.append(hideTypeButton(type));
  return cell;
}
// Hidden types are saved at once (also while downloading); downloaded files of that type stay listed.
async function hideFileType(type) {
  const count = state.files.filter(file => extension(file) === type && !file.downloaded).length;
  try {
    const result = await command('hideExtension', { extension: type });
    if (!result) return;
    filters.library.extension.delete(type);
    if ($('filters-dialog').open && filterContext === 'library') renderFilterGroups();
    renderFilterSummary('library');
    notify(`Hid ${count.toLocaleString()} ${type} file${count === 1 ? '' : 's'}`, 'Undo', async () => {
      try { await command('showExtension', { extension: type }); if ($('filters-dialog').open && filterContext === 'library') renderFilterGroups(); }
      catch (error) { fail(error.message); }
    });
  } catch (error) { fail(error.message); }
}
function tagKey(tag) { return tag.replace(/[A-Z]/g, character => character.toLowerCase()); }
function matchesChoices(selected, values) { return !selected.size || values.some(value => selected.has(value)); }
function matchesTags(context, tags = []) { return matchesChoices(filters[context].tags, tags.length ? tags.map(tagKey) : ['']); }
function libraryTags(file) {
  if (state.tagScope === 'file') return file.tags || [];
  if (state.tagScope === 'source') return file.sourceTags || [];
  return [...(file.sourceTags || []), ...(file.tags || [])];
}
function filterLabel(context, key) {
  if (context !== 'library' || key !== 'tags' || state.tagScope === 'all') return filterLabels[key];
  return state.tagScope === 'file' ? 'File tags' : 'Source tags';
}
function librarySource(file) { return file.collectionId ? `collection:${file.collectionId}` : file.item; }
const removedSource = 'source-removed:';
function filteredFiles() {
  const query = $('search').value.toLocaleLowerCase().trim();
  const selected = filters.library;
  return state.files.filter(file => (!query || file.name.toLocaleLowerCase().includes(query)) && matchesChoices(selected.source, [librarySource(file), ...(file.sourceRemoved ? [removedSource] : [])])
    && matchesChoices(selected.extension, [extension(file)]) && matchesTags('library', libraryTags(file)) && matchesChoices(selected.access, [file.restricted ? 'restricted' : 'public'])
    && matchesChoices(selected.favorite, [file.favorite ? 'favorite' : 'other'])
    && matchesChoices(selected.status, [file.downloaded ? 'downloaded' : 'missing', ...(queueStatus(file).queued ? ['queued'] : [])]))
    .sort((left, right) => {
      const sortValue = file => state.sort === 'name' ? filename(file) : ['tags', 'sourceTags'].includes(state.sort) ? (file[state.sort] || []).join(', ')
        : state.sort === 'status' ? queueStatus(file).text : state.sort === 'extension' ? extension(file) : file.collection || file.item;
      // A-Z on Size lists the largest files first.
      const compared = state.sort === 'size' ? right.size - left.size : state.sort === 'name'
        ? filename(left).localeCompare(filename(right), undefined, { numeric: true, sensitivity: 'base' })
        : sortValue(left).localeCompare(sortValue(right), undefined, { numeric: true, sensitivity: 'base' });
      return (compared || left.index - right.index) * (state.descending ? -1 : 1);
    });
}
function renderLibrary() {
  const filtered = filteredFiles();
  state.page = Math.min(state.page, Math.max(0, Math.ceil(filtered.length / pageSize) - 1));
  visible = filtered.slice(state.page * pageSize, (state.page + 1) * pageSize);
  const fragment = document.createDocumentFragment();
  for (const file of visible) {
    const displayName = filename(file);
    const row = node('tr'); row.classList.toggle('selected', state.selected.has(file.index));
    const check = node('input'); check.type = 'checkbox'; check.checked = state.selected.has(file.index);
    check.setAttribute('aria-label', `Select ${displayName}`);
    check.addEventListener('change', () => { if (check.checked) state.selected.add(file.index); else state.selected.delete(file.index); row.classList.toggle('selected', check.checked); selection(); });
    const selectCell = node('td'); selectCell.append(check); row.append(selectCell);
    const star = iconButton('star', `${file.favorite ? 'Remove favorite' : 'Favorite'}: ${displayName}`, async () => {
      try { await command('favorite', { index: file.index, revision: state.snapshot.catalogRevision }); await loadLibrary(true); }
      catch (error) { fail(error.message); }
    });
    star.className = `star-button${file.favorite ? ' favorited' : ''}`; star.setAttribute('aria-pressed', String(file.favorite));
    const favoriteCell = node('td'); favoriteCell.append(star); row.append(favoriteCell);
    const nameCell = node('td'); const title = node('div', undefined, 'filename');
    title.title = file.name;
    if (file.restricted) { const lock = icon('lock'); lock.title = 'Restricted'; title.append(lock); }
    title.append(node('span', displayName)); nameCell.append(title); row.append(nameCell, extensionCell(extension(file)));
    const sourceCell = node('td', file.collection || file.item, 'source-cell');
    if (file.collection) sourceCell.title = `${file.collection} (${file.item})`;
    row.append(sourceCell, tagsCell(file.sourceTags), tagsCell(file.tags, null, file), node('td', bytes(file.size), 'size-cell'));
    const status = queueStatus(file); const statusCell = node('td', undefined, 'status-cell');
    statusCell.append(node('span', status.text, `badge ${status.kind}`));
    if (!android && file.downloadPath) {
      const path = pathLabel(file.downloadPath, 30);
      if (file.downloadCopies?.length) path.title = file.downloadCopies.map(copy => `${copy.status}: ${copy.path}${copy.extracted ? `\nExtracted to: ${copy.extracted}` : ''}`).join('\n');
      statusCell.append(path);
    }
    if (!android && file.downloadPath && !status.queued && (file.downloaded || staleCopy(file)))
      statusCell.append(iconButton('trash', `${file.downloaded ? 'Delete downloaded file' : 'Remove download record'}: ${displayName}`, () => deleteDownload(file), busy()));
    row.append(statusCell); fragment.append(row);
  }
  $('library-body').replaceChildren(fragment); $('library-empty').hidden = filtered.length !== 0;
  $('library-summary').textContent = `${filtered.length.toLocaleString()} / ${state.files.length.toLocaleString()} files`;
  $('page-range').textContent = filtered.length ? `${state.page * pageSize + 1}-${Math.min((state.page + 1) * pageSize, filtered.length)} of ${filtered.length.toLocaleString()}` : '0 files';
  $('page-number').textContent = `${state.page + 1} / ${Math.max(1, Math.ceil(filtered.length / pageSize))}`;
  $('previous').disabled = state.page === 0; $('next').disabled = (state.page + 1) * pageSize >= filtered.length;
  applyColumnVisibility('library'); renderFilterSummary('library');
  selection(); updateControls();
}
function selection() {
  const count = visible.filter(file => state.selected.has(file.index)).length;
  $('select-page').checked = visible.length > 0 && count === visible.length;
  $('select-page').indeterminate = count > 0 && count < visible.length;
  updateControls();
}
