(function () {
    const selectedLeds = new Set();
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
        const sorted = Array.from(selectedLeds).sort((a, b) => a - b);
        selectedLedsInput.value = sorted.join(',');
    }

    function setLedSelected(btn, ledIndex, selected) {
        if (selected) {
            selectedLeds.add(ledIndex);
            btn.classList.remove('btn-outline-secondary');
            btn.classList.add('btn-warning');
        } else {
            selectedLeds.delete(ledIndex);
            btn.classList.remove('btn-warning');
            btn.classList.add('btn-outline-secondary');
        }
    }

    // Use delegated event listener on the persistent container to survive HTMX swaps
    persistentContainer.addEventListener('click', (e) => {
        const btn = e.target.closest('.led-btn');
        if (!btn) return;

        e.preventDefault();
        const ledIndex = parseInt(btn.dataset.ledIndex);

        console.log('LED click:', ledIndex, 'shift:', e.shiftKey, 'lastClicked:', lastClickedIndex);

        if (e.shiftKey && lastClickedIndex !== null) {
            const rangeStart = Math.min(lastClickedIndex, ledIndex);
            const rangeEnd = Math.max(lastClickedIndex, ledIndex);
            const selecting = !selectedLeds.has(ledIndex);
            console.log('Shift-click range:', rangeStart, '-', rangeEnd, 'selecting:', selecting);

            // Update all buttons in the range
            document.querySelectorAll('.led-btn').forEach(rangeBtn => {
                const idx = parseInt(rangeBtn.dataset.ledIndex);
                if (idx >= rangeStart && idx <= rangeEnd) {
                    setLedSelected(rangeBtn, idx, selecting);
                }
            });
        } else {
            setLedSelected(btn, ledIndex, !selectedLeds.has(ledIndex));
            lastClickedIndex = ledIndex;
        }

        console.log('Selected LEDs:', Array.from(selectedLeds));
        updateSelectedLedsInput();
        console.log('Hidden input value:', selectedLedsInput.value);

        // Send the full list to server without expecting a response
        htmx.ajax('POST', '/named_range/set', {
            values: { selected_leds: selectedLedsInput.value },
            swap: 'none'
        });
    });

    // Delegated handlers for clear and back buttons
    persistentContainer.addEventListener('click', (e) => {
        if (e.target.closest('#clear-btn')) {
            e.preventDefault();
            selectedLeds.clear();
            document.querySelectorAll('.led-btn').forEach(btn => {
                btn.classList.remove('btn-warning');
                btn.classList.add('btn-outline-secondary');
            });
            updateSelectedLedsInput();
            // Send empty list to server
            htmx.ajax('POST', '/named_range/set', {
                values: { selected_leds: '' },
                swap: 'none'
            });
        }

        if (e.target.closest('#back-btn')) {
            e.preventDefault();
            htmx.ajax('GET', '/named_range', { target: '#led-picker', swap: 'outerHTML' });
        }
    });

    // Sync the selected LEDs list before form submission
    if (rangeForm) {
        rangeForm.addEventListener('htmx:beforeRequest', () => {
            updateSelectedLedsInput();
        });
    }

    // Initialize selectedLeds from button states
    document.querySelectorAll('.led-btn').forEach(btn => {
        if (btn.classList.contains('btn-warning')) {
            selectedLeds.add(parseInt(btn.dataset.ledIndex));
        }
    });
    updateSelectedLedsInput();
})();
