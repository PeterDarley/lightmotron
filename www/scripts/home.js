function setActiveScene(sceneName, clickedButton, isOngoing) {
    document.getElementById('active-scene-name').textContent = sceneName;

    // Clear active state from all ongoing buttons.
    document.querySelectorAll('.ongoing-scene-btn').forEach(function(btn) {
        btn.classList.remove('btn-primary');
        btn.classList.add('btn-outline-primary');
    });

    // Highlight the clicked button only if it is an ongoing scene.
    if (isOngoing) {
        clickedButton.classList.remove('btn-outline-primary');
        clickedButton.classList.add('btn-primary');
    }
}
