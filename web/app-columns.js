'use strict';

function columnHeaders(context) {
  return [...document.querySelectorAll(`.${context}-table thead th`)];
}
function columnKey(header, context) {
  return context === 'library' ? header.dataset.libraryColumn : header.dataset.sourceColumn;
}
function applyColumnVisibility(context) {
  const headers = columnHeaders(context), rows = document.querySelectorAll(`#${context}-body tr`);
  headers.forEach((header, index) => {
    const key = columnKey(header, context), hidden = !!key && key !== 'name' && hiddenColumns[context].has(key);
    header.hidden = hidden;
    rows.forEach(row => { if (row.children[index]) row.children[index].hidden = hidden; });
  });
  applyColumnWidths(context);
}
function widthKey(header, context) {
  return columnKey(header, context) || `fixed-${header.cellIndex}`;
}
function applyColumnWidths(context) {
  if (!Object.keys(columnWidths[context]).length) return;
  const headers = columnHeaders(context), table = headers[0].closest('table');
  let total = 0;
  headers.forEach(header => {
    const key = widthKey(header, context), width = columnWidths[context][key] || (columnKey(header, context) ? 160 : 40);
    header.style.width = `${width}px`;
    const handle = header.querySelector('.column-resizer');
    if (handle) handle.setAttribute('aria-valuenow', String(Math.round(width)));
    if (!header.hidden) total += width;
  });
  table.style.minWidth = '0'; table.style.width = `${total}px`;
}
function captureColumnWidths(context) {
  columnHeaders(context).filter(header => !header.hidden).forEach(header => {
    columnWidths[context][widthKey(header, context)] = header.getBoundingClientRect().width;
  });
}
function saveColumnWidths() {
  try { localStorage.setItem('archivefetch-column-widths', JSON.stringify(columnWidths)); } catch {}
}
// Clicking a column header sorts by it; clicking the sorted column again reverses the direction.
for (const context of ['sources', 'library']) {
  columnHeaders(context).filter(header => columnKey(header, context)).forEach(header => {
    const label = header.textContent.trim(), button = node('button', label, 'sort-header');
    button.type = 'button'; button.title = `Sort by ${label}`;
    button.addEventListener('click', () => {
      const sorting = context === 'library' ? state : sourceSort, key = columnKey(header, context);
      sorting.descending = sorting.sort === key ? !sorting.descending : false; sorting.sort = key;
      applyFilters(context);
    });
    header.replaceChildren(button);
  });
}
for (const context of ['sources', 'library']) {
  columnHeaders(context).filter(header => columnKey(header, context)).forEach(header => {
    const handle = node('span', undefined, 'column-resizer');
    handle.tabIndex = 0; handle.setAttribute('role', 'separator'); handle.setAttribute('aria-orientation', 'vertical');
    handle.setAttribute('aria-label', `Resize ${header.textContent.trim()} column`);
    handle.setAttribute('aria-valuemin', '80'); handle.setAttribute('aria-valuemax', '1600');
    handle.title = `Resize ${header.textContent.trim()} column`;
    const resize = width => {
      columnWidths[context][widthKey(header, context)] = Math.max(80, Math.min(1600, width));
      applyColumnWidths(context);
    };
    let drag = null;
    handle.addEventListener('pointerdown', event => {
      if (event.button !== 0) return;
      event.preventDefault(); event.stopPropagation();
      captureColumnWidths(context);
      drag = { start: event.clientX, width: header.getBoundingClientRect().width };
      handle.setPointerCapture(event.pointerId); header.closest('table').classList.add('resizing-columns');
    });
    handle.addEventListener('pointermove', event => { if (drag) resize(drag.width + event.clientX - drag.start); });
    const finish = () => {
      if (!drag) return;
      drag = null; header.closest('table').classList.remove('resizing-columns'); saveColumnWidths();
    };
    handle.addEventListener('pointerup', event => {
      finish(); if (handle.hasPointerCapture(event.pointerId)) handle.releasePointerCapture(event.pointerId);
    });
    handle.addEventListener('pointercancel', finish); handle.addEventListener('lostpointercapture', finish);
    handle.addEventListener('click', event => event.stopPropagation());
    handle.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight'].includes(event.key)) return;
      event.preventDefault(); event.stopPropagation(); captureColumnWidths(context);
      resize(header.getBoundingClientRect().width + (event.key === 'ArrowRight' ? 1 : -1) * (event.shiftKey ? 64 : 16));
      saveColumnWidths();
    });
    header.append(handle);
  });
}
function saveColumnVisibility() {
  try { localStorage.setItem('archivefetch-hidden-columns', JSON.stringify(Object.fromEntries(Object.entries(hiddenColumns).map(([context, values]) => [context, [...values]])))); }
  catch {}
  applyColumnVisibility(columnContext);
}
function renderColumnOptions() {
  const context = columnContext;
  $('column-options').replaceChildren(...columnHeaders(context).filter(header => columnKey(header, context) && columnKey(header, context) !== 'name').map(header => {
    const key = columnKey(header, context), label = node('label', undefined, 'check'), check = node('input');
    check.type = 'checkbox'; check.checked = !hiddenColumns[context].has(key);
    check.addEventListener('change', () => {
      if (check.checked) hiddenColumns[context].delete(key); else hiddenColumns[context].add(key);
      saveColumnVisibility();
    });
    label.append(check, node('span', header.textContent)); return label;
  }));
}
document.querySelectorAll('[data-columns]').forEach(button => button.addEventListener('click', () => {
  columnContext = button.dataset.columns;
  $('columns-title').textContent = columnContext === 'library' ? 'Library columns' : 'Source columns';
  renderColumnOptions(); $('columns-dialog').showModal();
}));
$('reset-columns').addEventListener('click', () => {
  hiddenColumns[columnContext].clear(); columnWidths[columnContext] = {};
  const headers = columnHeaders(columnContext), table = headers[0].closest('table');
  table.style.removeProperty('width'); table.style.removeProperty('min-width');
  headers.forEach(header => { header.style.removeProperty('width'); header.querySelector('.column-resizer')?.removeAttribute('aria-valuenow'); });
  saveColumnWidths(); saveColumnVisibility(); renderColumnOptions();
});
function filterChoices(context, key) {
  const sources = state.snapshot?.sources || [];
  let choices;
  if (key === 'source') {
    choices = context === 'sources' ? sources.map(source => [source.item, source.name || source.item])
      : [...new Map(state.files.map(file => [librarySource(file), file.collection || sources.find(source => source.item === file.item)?.name || file.item]))];
    if (context === 'library' && state.files.some(file => file.sourceRemoved)) choices.push([removedSource, 'Source removed']);
  } else if (key === 'extension') {
    choices = state.files.map(file => { const value = extension(file); return [value, value || 'No extension']; });
  } else if (key === 'tags') {
    const tags = context === 'sources' ? sources.flatMap(source => source.tags || []) : state.files.flatMap(libraryTags);
    choices = [['', 'Untagged'], ...tags.map(tag => [tagKey(tag), tag])];
  } else if (key === 'access') choices = [['public', 'Public'], ['restricted', 'Restricted']];
  else if (key === 'favorite') choices = [['favorite', 'Favorites'], ['other', 'Not favorited']];
  else choices = [['downloaded', 'Downloaded'], ['missing', 'Not downloaded'], ['queued', 'In queue']];
  const unique = new Map();
  for (const [value, label] of choices) if (!unique.has(value)) unique.set(value, label);
  for (const value of filters[context][key]) if (!unique.has(value)) unique.set(value, value || (key === 'extension' ? 'No extension' : 'Untagged'));
  return [...unique].sort((left, right) => left[1].localeCompare(right[1], undefined, { numeric: true, sensitivity: 'base' }));
}
function clearFilters(context) {
  Object.values(filters[context]).forEach(selected => selected.clear());
  if (context === 'library') state.tagScope = 'all';
  applyFilters(context);
  if ($('filters-dialog').open && filterContext === context) renderFilterGroups();
}
function applyFilters(context) {
  if (context === 'library') { state.page = 0; renderLibrary(); } else renderSources();
}
function renderFilterSummary(context) {
  const fragment = document.createDocumentFragment();
  let count = 0;
  for (const [key, selected] of Object.entries(filters[context])) {
    const labels = new Map(filterChoices(context, key));
    for (const value of selected) {
      ++count;
      const label = `${filterLabel(context, key)}: ${labels.get(value)}`;
      const chip = node('button', label, 'filter-chip'); chip.title = `Remove ${label}`; chip.setAttribute('aria-label', chip.title); chip.append(icon('close'));
      chip.addEventListener('click', () => { selected.delete(value); applyFilters(context); }); fragment.append(chip);
    }
  }
  $(`${context}-active-filters`).replaceChildren(fragment);
  $(`${context}-filter-count`).textContent = count ? `(${count})` : '';
  $(`${context}-clear-filters`).hidden = count === 0;
  const sorting = context === 'library' ? state : sourceSort;
  document.querySelectorAll(`[data-${context === 'library' ? 'library' : 'source'}-column]`).forEach(header => {
    const column = context === 'library' ? header.dataset.libraryColumn : header.dataset.sourceColumn;
    header.setAttribute('aria-sort', column === sorting.sort ? (sorting.descending ? 'descending' : 'ascending') : 'none');
  });
}
function openFilters(context) {
  filterContext = context;
  $('filters-title').textContent = context === 'library' ? 'Library filters' : 'Source filters';
  const sorting = context === 'library' ? state : sourceSort;
  const columns = context === 'library' ? [['name', 'Filename'], ['extension', 'Extension'], ['item', 'Source'], ['sourceTags', 'Source Tags'], ['tags', 'Tags'], ['size', 'Size'], ['status', 'Status']]
    : [['name', 'Source'], ['tags', 'Tags'], ['scanned', 'Last scanned']];
  $('filter-sort').replaceChildren(...columns.map(([value, label]) => new Option(label, value)));
  $('filter-sort').value = sorting.sort;
  document.querySelectorAll('[name="sort-direction"]').forEach(radio => { radio.checked = (radio.value === 'descending') === sorting.descending; });
  renderFilterGroups(); $('filters-dialog').showModal();
}
function closeFilterDropdowns(except = null) {
  document.querySelectorAll('.multi-select').forEach(dropdown => {
    if (dropdown === except) return;
    dropdown.querySelector('.multi-select-panel').hidden = true;
    const input = dropdown.querySelector('[role="combobox"]');
    input.setAttribute('aria-expanded', 'false'); input.removeAttribute('aria-activedescendant'); input.value = '';
  });
}
function renderFilterGroups() {
  const context = filterContext, fragment = document.createDocumentFragment();
  for (const [key, selected] of Object.entries(filters[context])) {
    const group = node('fieldset'); group.append(node('legend', filterLabels[key]));
    if (context === 'library' && key === 'tags') {
      const scope = node('div', undefined, 'tag-scope'); scope.setAttribute('role', 'radiogroup'); scope.setAttribute('aria-label', 'Match tags on');
      for (const [value, label] of [['all', 'All tags'], ['file', 'File tags'], ['source', 'Source tags']]) {
        const choice = node('label', undefined, 'check'), radio = node('input');
        radio.type = 'radio'; radio.name = 'tag-scope'; radio.value = value; radio.checked = state.tagScope === value;
        radio.addEventListener('change', () => { state.tagScope = value; applyFilters('library'); renderFilterGroups(); });
        choice.append(radio, document.createTextNode(label)); scope.append(choice);
      }
      group.append(scope);
    }
    const dropdown = node('div', undefined, 'multi-select'), control = node('div', undefined, 'multi-select-control');
    const input = node('input'); input.type = 'text'; input.autocomplete = 'off'; input.setAttribute('role', 'combobox');
    input.setAttribute('aria-label', filterLabels[key]); input.setAttribute('aria-autocomplete', 'list'); input.setAttribute('aria-expanded', 'false');
    const panel = node('div', undefined, 'multi-select-panel'); panel.hidden = true;
    const list = node('div', undefined, 'filter-options'); list.id = `options-${context}-${key}`;
    list.setAttribute('role', 'listbox'); list.setAttribute('aria-label', filterLabels[key]); list.setAttribute('aria-multiselectable', 'true');
    input.setAttribute('aria-controls', list.id);
    const choices = filterChoices(context, key), labels = new Map(choices);
    let active = -1;
    const summary = () => {
      input.placeholder = selected.size ? [...selected].map(value => labels.get(value)).join(', ') : `Any ${filterLabels[key].toLowerCase()}`;
      input.title = input.placeholder; dropdown.classList.toggle('has-selection', !!selected.size);
    };
    const open = () => { closeFilterDropdowns(dropdown); panel.hidden = false; input.setAttribute('aria-expanded', 'true'); };
    const empty = node('div', 'No matches', 'dropdown-empty'); empty.setAttribute('role', 'status');
    const narrow = () => {
      const query = input.value.toLocaleLowerCase().trim(); active = -1; input.removeAttribute('aria-activedescendant');
      for (const option of list.children) { option.hidden = !option.dataset.search.includes(query); option.classList.remove('active'); }
      empty.hidden = [...list.children].some(option => !option.hidden);
    };
    for (const [value, label] of choices) {
      const option = node('div', undefined, 'multi-select-option'); option.dataset.search = `${label} ${value}`.toLocaleLowerCase();
      option.id = `${list.id}-${list.children.length}`; option.setAttribute('role', 'option'); option.setAttribute('aria-selected', String(selected.has(value)));
      const check = node('span', undefined, 'option-check'); check.setAttribute('aria-hidden', 'true'); check.append(icon('clear'));
      option.append(check, node('span', label)); if (key === 'source') option.title = value;
      if (context === 'library' && key === 'extension' && value) option.append(hideTypeButton(value));
      option.addEventListener('mousedown', event => event.preventDefault());
      option.addEventListener('click', () => {
        if (selected.has(value)) selected.delete(value); else selected.add(value);
        option.setAttribute('aria-selected', String(selected.has(value)));
        input.value = ''; narrow(); summary(); applyFilters(context); input.focus();
      });
      list.append(option);
    }
    const toggle = iconButton('chevron', `Open ${filterLabels[key].toLowerCase()}`, () => {
      if (panel.hidden) { input.value = ''; narrow(); open(); } else closeFilterDropdowns();
      input.focus();
    });
    toggle.tabIndex = -1;
    input.addEventListener('click', () => { narrow(); open(); });
    input.addEventListener('input', () => { narrow(); open(); });
    input.addEventListener('keydown', event => {
      if (event.key === 'Escape' && !panel.hidden) { event.preventDefault(); event.stopPropagation(); closeFilterDropdowns(); }
      else if (event.key === 'Tab') closeFilterDropdowns();
      else if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
        event.preventDefault(); if (panel.hidden) narrow(); open();
        const options = [...list.children].filter(option => !option.hidden);
        list.querySelector('.active')?.classList.remove('active');
        active = event.key === 'ArrowDown' ? Math.min(options.length - 1, active + 1) : active <= 0 ? options.length - 1 : active - 1;
        const option = options[active];
        if (option) { option.classList.add('active'); input.setAttribute('aria-activedescendant', option.id); option.scrollIntoView({ block: 'nearest' }); }
      } else if (event.key === 'Enter' && !panel.hidden) {
        event.preventDefault(); [...list.children].filter(option => !option.hidden)[active < 0 ? 0 : active]?.click();
      }
    });
    summary(); narrow(); control.append(input, toggle); panel.append(list, empty); dropdown.append(control, panel);
    group.append(dropdown); fragment.append(group);
  }
  $('filter-groups').replaceChildren(fragment);
}
