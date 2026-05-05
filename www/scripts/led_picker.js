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

    async function syncWithServer() {
        updateSelectedLedsInput();
        try {
            const resp = await fetch('/named_range/set', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams({ selected_leds: selectedLedsInput.value })
            });
            if (resp.ok) {
                const arr = await resp.json();
                if (Array.isArray(arr)) {
                    highlightIndices(arr);
                }
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

        syncWithServer();
    });

    // Delegated handlers for clear and back buttons and add-range button
    persistentContainer.addEventListener('click', (e) => {
        if (e.target.closest('#clear-btn')) {
            e.preventDefault();
            selectedItems.clear();
            syncWithServer();
        }

        if (e.target.closest('#back-btn')) {
            e.preventDefault();
            document.getElementById('modal-body').innerHTML = '<div class="text-center py-4"><div class="spinner-border text-primary" role="status"><span class="visually-hidden">Loading...</span></div></div>';
        }

        if (e.target.closest('#add-range-btn')) {
            e.preventDefault();
            const sel = document.getElementById('named-range-select');
            if (sel && sel.value) {
                selectedItems.add(sel.value);
                syncWithServer();
            }
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
    syncWithServer();
})();
