'use strict';

function filteredSources() {
  const query = $('source-search').value.toLocaleLowerCase().trim();
  return (state.snapshot?.sources || []).map((source, index) => ({ ...source, index }))
    .filter(source => (!query || `${source.name} ${source.url} ${collectionById(source.collection)?.name || ''}`.toLocaleLowerCase().includes(query))
      && matchesChoices(filters.sources.source, [source.item]) && matchesTags('sources', source.tags))
    .sort((left, right) => {
      const value = source => sourceSort.sort === 'tags' ? (source.tags || []).join(', ') : source[sourceSort.sort] || '';
      return (value(left).localeCompare(value(right), undefined, { numeric: true, sensitivity: 'base' }) || left.index - right.index) * (sourceSort.descending ? -1 : 1);
    });
}
function collectionById(id) { return id ? (state.snapshot?.collections || []).find(collection => collection.id === id) : null; }
function tagsCell(tags = [], source = null, file = null) {
  const cell = node('td', undefined, 'tags-cell');
  const list = node('div', undefined, 'tag-list');
  for (const tag of tags) list.append(node('span', tag, 'tag'));
  if (source) list.append(iconButton('tag', `Edit tags for ${source.name || source.item}`, () => editTags(source), sourceBusy()));
  if (file) list.append(iconButton('tag', `Edit tags for ${filename(file)}`, () => editTags(null, file), busy()));
  if (!tags.length && !source && !file) list.append(node('span', '-', 'muted'));
  cell.append(list); return cell;
}
function sourceRow(source, collection = null) {
  const index = source.index;
  const row = node('tr', undefined, collection ? 'member-row' : ''); const selectCell = node('td'); const check = node('input'); check.type = 'checkbox'; check.checked = source.selected;
  check.setAttribute('aria-label', `Select source ${source.name}`); check.addEventListener('change', () => run('selectSource', { index, selected: check.checked })); selectCell.append(check);
  const nameCell = node('td'); nameCell.append(node('span', source.name || source.url), node('span', source.url, 'secondary'));
  if (source.error) nameCell.append(node('span', source.error, 'secondary error'));
  const actionCell = node('td', undefined, 'action-cell');
  if (collection) actionCell.append(iconButton('up', `Remove ${source.name} from ${collection.name}`, () => openCollectionDialog('remove', { id: collection.id, urls: [source.url] }), sourceBusy()));
  actionCell.append(iconButton('trash', `Remove source ${source.name}`, () => run('removeSource', { index }), sourceBusy()));
  // Sources in a collection use the collection's tags, so they get no tag button.
  row.append(selectCell, nameCell, tagsCell(source.tags, collection ? null : source), node('td', source.scanned || 'Not scanned', 'scanned-cell'), actionCell);
  return row;
}
function collectionRow(collection, members, open, filtering) {
  const selected = members.filter(source => source.selected).length;
  const row = node('tr', undefined, 'collection-row'); const selectCell = node('td'); const check = node('input'); check.type = 'checkbox';
  check.checked = !!members.length && selected === members.length; check.indeterminate = selected > 0 && selected < members.length; check.disabled = !members.length;
  check.setAttribute('aria-label', `Select sources in ${collection.name}`);
  check.addEventListener('change', () => run('selectSources', { selected: check.checked, urls: members.map(source => source.url) }));
  selectCell.append(check);
  const toggle = node('button', undefined, 'collection-toggle'); toggle.type = 'button'; toggle.disabled = filtering;
  toggle.setAttribute('aria-expanded', String(open));
  toggle.append(icon(open ? 'chevron' : 'next'), node('span', collection.name, 'collection-name'));
  toggle.addEventListener('click', () => {
    if (open) openCollections.delete(collection.id); else openCollections.add(collection.id);
    saveOpenCollections(); renderSources();
  });
  const nameCell = node('td'), heading = node('div', undefined, 'collection-cell');
  heading.append(toggle, iconButton('settings', `Edit collection ${collection.name}`, () => openCollectionDialog('edit', { id: collection.id }), sourceBusy()));
  nameCell.append(heading);
  const count = `${members.length} source${members.length === 1 ? '' : 's'}${selected ? `, ${selected} selected` : ''}`;
  row.append(selectCell, nameCell, tagsCell(collection.tags), node('td', count, 'scanned-cell muted'), node('td', undefined, 'action-cell'));
  return row;
}
function renderSources() {
  const sources = filteredSources();
  const fragment = document.createDocumentFragment();
  const collections = [...(state.snapshot?.collections || [])].sort((left, right) => left.name.localeCompare(right.name, undefined, { numeric: true, sensitivity: 'base' }));
  if (!collections.length) sources.forEach(source => fragment.append(sourceRow(source)));
  else {
    const filtering = !!$('source-search').value.trim() || Object.values(filters.sources).some(selected => selected.size);
    for (const collection of collections) {
      const members = sources.filter(source => source.collection === collection.id);
      if (filtering && !members.length) continue;
      const open = filtering || openCollections.has(collection.id);
      fragment.append(collectionRow(collection, members, open, filtering));
      if (open) members.forEach(source => fragment.append(sourceRow(source, collection)));
    }
    const loose = sources.filter(source => !collectionById(source.collection));
    if (loose.length) {
      const heading = node('tr', undefined, 'collection-row loose-row');
      const label = node('td'); label.append(node('div', 'Not in a collection', 'collection-cell muted'));
      heading.append(node('td'), label, node('td'), node('td', undefined, 'scanned-cell'), node('td', undefined, 'action-cell'));
      fragment.append(heading);
      loose.forEach(source => fragment.append(sourceRow(source)));
    }
  }
  $('sources-body').replaceChildren(fragment); $('sources-empty').hidden = sources.length !== 0;
  const count = sources.filter(source => source.selected).length;
  $('selected-sources').textContent = `${sources.length} shown / ${(state.snapshot?.sources || []).filter(source => source.selected).length} selected`;
  $('all-sources').checked = !!sources.length && count === sources.length; $('all-sources').indeterminate = count > 0 && count < sources.length;
  applyColumnVisibility('sources'); renderFilterSummary('sources');
}
