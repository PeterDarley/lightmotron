(function () {
    const persistentContainer = document.getElementById('custom-colors-picker');
    if (!persistentContainer) {
        console.error('custom-colors-picker not found');
        return;
    }

    // Delegated handler for delete buttons
    persistentContainer.addEventListener('click', (e) => {
        const deleteBtn = e.target.closest('.delete-color-btn');
        if (!deleteBtn) return;

        e.preventDefault();
        const colorName = deleteBtn.dataset.colorName;

        // Send delete request to server and refresh the entire picker
        htmx.ajax('POST', '/custom_colors', {
            values: { color_name: colorName, action: 'delete' },
            target: '#custom-colors-picker',
            swap: 'outerHTML'
        });
    });
})();
