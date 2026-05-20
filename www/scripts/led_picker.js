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

    function highlightIndices(resolved) {
        // Clear all
        document.querySelectorAll('.led-btn').forEach(btn => {
            btn.classList.remove('btn-warning');
            btn.classList.add('btn-outline-secondary');
        });
        // Mark resolved
        resolved.forEach(idx => {
            const btn = document.querySelector('.led-btn[data-led-index="' + idx + '"]');
            if (btn) {
                btn.classList.remove('btn-outline-secondary');
                btn.classList.add('btn-warning');
            }
        });
    }

    async function syncWithServer(reason) {
        updateSelectedLedsInput();

        try {
            const resp = await fetch('/named_range/set', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams({ selected_leds: selectedLedsInput.value })
            });
            if (resp.ok) {
                const data = await resp.json();
                let directSelected = [];
                let resolvedSelected = [];

                if (Array.isArray(data)) {
                    // Backward compatibility with older endpoint response format.
                    directSelected = data;
                    resolvedSelected = data;
                } else if (data && typeof data === 'object') {
                    directSelected = Array.isArray(data.direct_selected_leds) ? data.direct_selected_leds : [];
                    resolvedSelected = Array.isArray(data.resolved_leds) ? data.resolved_leds : [];
                }

                highlightIndices(directSelected);
            } else {
                // on error, just clear highlights
                highlightIndices([]);
            }
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

        syncWithServer('led-button-click');
    });

    // Delegated handlers for clear and back buttons and add-range button
    persistentContainer.addEventListener('click', (e) => {
        // Subrange removal is handled by HTMX buttons in the template.

        if (e.target.closest('#clear-btn')) {
            e.preventDefault();
            selectedItems.clear();
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
                selectedItems.add(sel.value);
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

    // Kick off initial sync so server-side resolution is applied
    syncWithServer('initial-load');
})();
