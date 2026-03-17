// Doxygen firmware version & SKU filtering for note-cpp.
// Adapted from notecard-schema/scripts/preview_site.py filtering logic.

(function() {
'use strict';

// Known firmware versions (from OpenAPI spec x-min-api-version values).
var VERSIONS = [
    '9.1.1', '8.2.1', '7.5.2', '7.5.1', '7.3.1', '7.2.2', '7.2.1',
    '6.2.3', '6.1.1', '5.3.1', '5.1.1', '4.1.1',
    '3.5.1', '3.4.1', '3.3.1', '3.2.1'
];

// Known Notecard SKUs.
var SKUS = ['CELL', 'CELL+WIFI', 'WIFI', 'LORA', 'SKYLO'];

// Semantic version comparison: -1 if a < b, 0 if equal, 1 if a > b.
function cmpVer(a, b) {
    var pa = a.split('.').map(Number), pb = b.split('.').map(Number);
    for (var i = 0; i < Math.max(pa.length, pb.length); i++) {
        var va = pa[i] || 0, vb = pb[i] || 0;
        if (va < vb) return -1;
        if (va > vb) return 1;
    }
    return 0;
}

// Find the nearest Doxygen member container around an element.
// Doxygen structures member docs as:
//   <div class="memitem"> ... <div class="memdoc"> ... our span ... </div> </div>
// or inside a <tr> for table-based layouts.
function findMemberContainer(el) {
    var node = el;
    while (node && node !== document.body) {
        if (node.classList) {
            if (node.classList.contains('memitem')) return node;
            if (node.classList.contains('memdoc')) return node.parentElement;
        }
        if (node.tagName === 'TR') return node;
        node = node.parentElement;
    }
    return null;
}

// Apply version and SKU filters to the page.
function applyFilters() {
    var verSelect = document.getElementById('nc-filter-version');
    var ver = verSelect ? verSelect.value : '';
    var skuBoxes = document.querySelectorAll('#nc-filter-bar .sku-check input[type=checkbox]');
    var activeSkus = new Set();
    skuBoxes.forEach(function(cb) { if (cb.checked) activeSkus.add(cb.value); });

    // Track containers that should be hidden (a container may have multiple
    // annotated elements; hide only if ALL of them are filtered out).
    var containerState = new Map();

    document.querySelectorAll('[data-min-api-version], [data-skus]').forEach(function(el) {
        var hide = false;

        var minVer = el.getAttribute('data-min-api-version');
        if (ver && minVer && cmpVer(ver, minVer) < 0) {
            hide = true;
        }

        var elSkus = el.getAttribute('data-skus');
        if (elSkus && activeSkus.size > 0) {
            var arr = elSkus.split(',');
            var overlap = arr.some(function(s) { return activeSkus.has(s); });
            if (!overlap) hide = true;
        }

        // Apply to the annotated element itself (the dl.section badge).
        if (hide) {
            el.classList.add('filtered-out');
        } else {
            el.classList.remove('filtered-out');
        }

        // Also propagate to the member container.
        var container = findMemberContainer(el);
        if (container) {
            if (!containerState.has(container)) {
                containerState.set(container, { total: 0, hidden: 0 });
            }
            var state = containerState.get(container);
            state.total++;
            if (hide) state.hidden++;
        }
    });

    // Hide containers where ALL annotated elements are filtered out.
    containerState.forEach(function(state, container) {
        if (state.hidden > 0 && state.hidden >= state.total) {
            container.classList.add('filtered-out');
        } else {
            container.classList.remove('filtered-out');
        }
    });

    // Persist to localStorage.
    try {
        localStorage.setItem('nc-doxy-filter-ver', ver);
        localStorage.setItem('nc-doxy-filter-skus', JSON.stringify(Array.from(activeSkus)));
    } catch(e) {}
}

// Build and inject the filter bar into the page.
function injectFilterBar() {
    // Don't inject on the main page or file list pages — only on struct/class docs.
    var content = document.getElementById('doc-content') || document.querySelector('.contents');
    if (!content) return;

    // Only inject if the page has filterable elements.
    if (!document.querySelector('[data-min-api-version], [data-skus]')) return;

    var bar = document.createElement('div');
    bar.id = 'nc-filter-bar';

    // Version dropdown
    var verLabel = document.createElement('label');
    verLabel.textContent = 'Firmware:';
    verLabel.setAttribute('for', 'nc-filter-version');
    bar.appendChild(verLabel);

    var verSelect = document.createElement('select');
    verSelect.id = 'nc-filter-version';
    var allOpt = document.createElement('option');
    allOpt.value = '';
    allOpt.textContent = 'All versions';
    verSelect.appendChild(allOpt);
    VERSIONS.forEach(function(v) {
        var opt = document.createElement('option');
        opt.value = v;
        opt.textContent = v;
        verSelect.appendChild(opt);
    });
    verSelect.addEventListener('change', applyFilters);
    bar.appendChild(verSelect);

    // SKU checkboxes
    var skuLabel = document.createElement('label');
    skuLabel.textContent = 'SKU:';
    bar.appendChild(skuLabel);

    var skuGroup = document.createElement('span');
    skuGroup.className = 'sku-group';
    SKUS.forEach(function(sku) {
        var label = document.createElement('label');
        label.className = 'sku-check';
        var cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.value = sku;
        cb.checked = true;
        cb.addEventListener('change', applyFilters);
        label.appendChild(cb);
        label.appendChild(document.createTextNode(' ' + sku));
        skuGroup.appendChild(label);
    });
    bar.appendChild(skuGroup);

    // Insert at top of content area.
    content.insertBefore(bar, content.firstChild);
}

// Restore saved filter state from localStorage.
function restoreFilters() {
    try {
        var savedVer = localStorage.getItem('nc-doxy-filter-ver');
        var savedSkus = localStorage.getItem('nc-doxy-filter-skus');
        if (savedVer) {
            var verSelect = document.getElementById('nc-filter-version');
            if (verSelect) verSelect.value = savedVer;
        }
        if (savedSkus) {
            var skus = JSON.parse(savedSkus);
            var skuSet = new Set(skus);
            document.querySelectorAll('#nc-filter-bar .sku-check input[type=checkbox]').forEach(function(cb) {
                cb.checked = skuSet.has(cb.value);
            });
        }
    } catch(e) {}
}

document.addEventListener('DOMContentLoaded', function() {
    injectFilterBar();
    restoreFilters();
    applyFilters();
});

})();
