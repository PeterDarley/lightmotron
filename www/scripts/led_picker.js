(function () {
    const selectedItems = new Set(); // stores tokens: numbers as strings or named:refs
    const selectedLedsInput = document.getElementById('selected-leds');
    const rangeForm = document.getElementById('range-form');

    // Attach listener to persistent container that survives HTMX swaps
    const persistentContainer = document.getElementById('led-picker-container');
    if (!persistentContainer) {
        console.error('led-picker-container not found');
        return;
    }

    let lastClickedIndex = null;

    function updateSelectedLedsInput() {
        selectedLedsInput.value = Array.from(selectedItems).join(',');
    }

    function highlightFromLocalState() {
        // Derive direct LED indices from selectedItems and highlight immediately.
        const allBtns = document.querySelectorAll('.led-btn');
        allBtns.forEach(btn => {
            btn.classList.remove('btn-warning');
            btn.classList.add('btn-outline-secondary');
        });
        selectedItems.forEach(token => {
            const idx = parseInt(token);
            if (!isNaN(idx)) {
                const btn = document.querySelector('.led-btn[data-led-index="' + idx + '"]');
                if (btn) {
                    btn.classList.remove('btn-outline-secondary');
                    btn.classList.add('btn-warning');
                }
            }
        });
    }

    async function syncWithServer(reason) {
        updateSelectedLedsInput();

        try {
            await fetch('/named_range/set', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams({ selected_leds: selectedLedsInput.value })
            });
        } catch (e) {
            console.error('named_range sync failed', e);
        }
    }

    function toggleIndex(idx) {
        const token = String(idx);
        if (selectedItems.has(token)) {
            selectedItems.delete(token);
        } else {
            selectedItems.add(token);
        }
    }

    function getSortBy() {
        const ledPicker = document.getElementById('led-picker');
        if (!ledPicker || !ledPicker.dataset || !ledPicker.dataset.sortBy) {
            return 'name';
        }

        return ledPicker.dataset.sortBy;
    }

    function ensureIncludedSubrangesList() {
        let list = document.getElementById('included-subranges-list');
        if (list) {
            return list;
        }

        const includeRangeSelect = document.getElementById('named-range-select');
        if (!includeRangeSelect) {
            return null;
        }

        const includeRangeBlock = includeRangeSelect.closest('.mb-3');
        if (!includeRangeBlock || !includeRangeBlock.parentNode) {
            return null;
        }

        const section = document.createElement('div');
        section.className = 'mb-3';
        section.id = 'included-subranges-section';

        const label = document.createElement('small');
        label.className = 'text-muted';
        label.textContent = 'Included subranges:';
        section.appendChild(label);

        list = document.createElement('div');
        list.className = 'd-flex flex-wrap gap-1 mt-1';
        list.id = 'included-subranges-list';
        section.appendChild(list);

        includeRangeBlock.parentNode.insertBefore(section, includeRangeBlock);
        return list;
    }

    function addSubrangeChip(token) {
        if (!token || !token.startsWith('named:')) {
            return;
        }

        const existingChip = document.querySelector('[data-subrange-chip="' + token + '"]');
        if (existingChip) {
            return;
        }

        const list = ensureIncludedSubrangesList();
        if (!list) {
            return;
        }

        const subrangeName = token.slice(6);
        if (!subrangeName) {
            return;
        }

        const chip = document.createElement('span');
        chip.className = 'badge bg-secondary';
        chip.setAttribute('data-subrange-chip', token);
        chip.appendChild(document.createTextNode(subrangeName + ' '));

        const removeButton = document.createElement('button');
        removeButton.type = 'button';
        removeButton.className = 'btn btn-outline-light btn-sm ms-2 py-0 px-1';
        removeButton.setAttribute('data-subrange-token', token);
        removeButton.setAttribute('hx-post', '/named_range/remove_subrange?token=' + token + '&sort=' + getSortBy());
        removeButton.setAttribute('hx-target', '#modal-body');
        removeButton.setAttribute('hx-swap', 'innerHTML');
        removeButton.setAttribute('aria-label', 'Remove ' + subrangeName);
        removeButton.textContent = 'x';
        chip.appendChild(removeButton);

        list.appendChild(chip);
        if (window.htmx && typeof window.htmx.process === 'function') {
            window.htmx.process(chip);
        }
    }

    // Use delegated event listener on the persistent container to survive HTMX swaps
    persistentContainer.addEventListener('click', (e) => {
        const btn = e.target.closest('.led-btn');
        if (!btn) return;

        e.preventDefault();
        const ledIndex = parseInt(btn.dataset.ledIndex);

        if (e.shiftKey && lastClickedIndex !== null) {
            const rangeStart = Math.min(lastClickedIndex, ledIndex);
            const rangeEnd = Math.max(lastClickedIndex, ledIndex);
            const selecting = !selectedItems.has(String(ledIndex));
            for (let idx = rangeStart; idx <= rangeEnd; idx++) {
                if (selecting) selectedItems.add(String(idx)); else selectedItems.delete(String(idx));
            }
        } else {
            toggleIndex(ledIndex);
            lastClickedIndex = ledIndex;
        }

        highlightFromLocalState();
        syncWithServer('led-button-click');
    });

    // Delegated handlers for clear and back buttons and add-range button
    persistentContainer.addEventListener('click', (e) => {
        // Subrange removal is handled by HTMX buttons in the template.

        if (e.target.closest('#clear-btn')) {
            e.preventDefault();
            selectedItems.clear();
            highlightFromLocalState();
            syncWithServer('clear');
            return;
        }

        if (e.target.closest('#back-btn')) {
            e.preventDefault();
            document.getElementById('modal-body').innerHTML = '<div class="text-center py-4"><div class="spinner-border text-primary" role="status"><span class="visually-hidden">Loading...</span></div></div>';
            return;
        }

        if (e.target.closest('#add-range-btn')) {
            e.preventDefault();
            const sel = document.getElementById('named-range-select');
            if (sel && sel.value) {
                const selectedToken = sel.value;
                selectedItems.add(selectedToken);
                addSubrangeChip(selectedToken);
                sel.value = '';
                highlightFromLocalState();
                syncWithServer('subrange-add');
            }
            return;
        }
    });

    // Sync the selected LEDs list before form submission
    if (rangeForm) {
        rangeForm.addEventListener('htmx:beforeRequest', () => {
            updateSelectedLedsInput();
        });
    }

    // Initialize selectedItems from hidden input
    try {
        const initial = selectedLedsInput.value || '';
        initial.split(',').map(s => s.trim()).filter(Boolean).forEach(t => selectedItems.add(t));
    } catch (e) {
        // ignore
    }

    // Highlight from local state on load, then sync with server for hardware
    highlightFromLocalState();
    syncWithServer('initial-load');
})();
