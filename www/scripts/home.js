function toggleActiveScene(sceneName, clickedButton) {
    var isNowActive = clickedButton.classList.contains('btn-outline-primary');

    if (isNowActive) {
        // Activating: switch to filled style.
        clickedButton.classList.remove('btn-outline-primary');
        clickedButton.classList.add('btn-primary');
        // Update hx-vals so next click removes.
        clickedButton.setAttribute('hx-vals', JSON.stringify({scene: sceneName, action: 'remove'}));
    } else {
        // Deactivating: switch to outline style.
        clickedButton.classList.remove('btn-primary');
        clickedButton.classList.add('btn-outline-primary');
        // Update hx-vals so next click adds.
        clickedButton.setAttribute('hx-vals', JSON.stringify({scene: sceneName, action: 'add'}));
    }

    // Update the active scene label to show all active scenes.
    var activeScenes = Array.from(document.querySelectorAll('.theme-ongoing-btn.btn-primary'))
        .map(function(btn) { return btn.dataset.scene; });
    var label = activeScenes.length > 0 ? activeScenes.join(', ') : '—';
    var nameEl = document.getElementById('active-scene-name');
    if (nameEl) { nameEl.textContent = label; }
}
