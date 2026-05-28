/**
 * Sound playback module
 * Reads sound file list from CSS variables and pre-loads audio files
 */

class SoundManager {
    constructor() {
        this.audioPool = [];
        this.closeSoundPool = [];
        this.navSoundPool = [];
        this.initialized = false;
        this.closeInitialized = false;
        this.navInitialized = false;
        this.init();
        this.initCloseSounds();
        this.initNavSounds();
    }

    init() {
        // Read sound files from CSS variable
        const root = document.documentElement;
        const cssValue = getComputedStyle(root).getPropertyValue('--sound-files').trim();

        if (!cssValue) {
            return;
        }

        // Parse pipe-separated list, remove quotes, and filter empty strings
        const soundFiles = cssValue
            .split('|')
            .map(file => file.trim().replace(/^["']|["']$/g, ''))
            .filter(file => file.length > 0);

        // Pre-load audio files
        soundFiles.forEach(file => {
            const audio = new Audio(`/sounds/${file}`);
            audio.preload = 'auto';
            this.audioPool.push(audio);
        });

        this.initialized = true;
    }

    initCloseSounds() {
        // Read close button sound files from CSS variable
        const root = document.documentElement;
        const cssValue = getComputedStyle(root).getPropertyValue('--sound-files-close').trim();

        if (!cssValue) {
            return;
        }

        // Parse pipe-separated list, remove quotes, and filter empty strings
        const soundFiles = cssValue
            .split('|')
            .map(file => file.trim().replace(/^["']|["']$/g, ''))
            .filter(file => file.length > 0);

        // Pre-load audio files
        soundFiles.forEach(file => {
            const audio = new Audio(`/sounds/${file}`);
            audio.preload = 'auto';
            this.closeSoundPool.push(audio);
        });

        this.closeInitialized = true;
    }

    initNavSounds() {
        // Read nav link sound files from CSS variable
        const root = document.documentElement;
        const cssValue = getComputedStyle(root).getPropertyValue('--sound-files-nav').trim();

        if (!cssValue) {
            return;
        }

        // Parse pipe-separated list, remove quotes, and filter empty strings
        const soundFiles = cssValue
            .split('|')
            .map(file => file.trim().replace(/^["']|["']$/g, ''))
            .filter(file => file.length > 0);

        // Pre-load audio files
        soundFiles.forEach(file => {
            const audio = new Audio(`/sounds/${file}`);
            audio.preload = 'auto';
            this.navSoundPool.push(audio);
        });

        this.navInitialized = true;
    }

    play() {
        if (!this.initialized || this.audioPool.length === 0) {
            return;
        }

        // Select random file
        const randomIndex = Math.floor(Math.random() * this.audioPool.length);
        const audio = this.audioPool[randomIndex];

        // Reset to start and play
        audio.currentTime = 0;
        audio.play().catch(err => {
            console.warn('Audio playback failed:', err);
        });
    }

    playCloseSound() {
        if (!this.closeInitialized || this.closeSoundPool.length === 0) {
            return;
        }

        // Select random file
        const randomIndex = Math.floor(Math.random() * this.closeSoundPool.length);
        const audio = this.closeSoundPool[randomIndex];

        // Reset to start and play
        audio.currentTime = 0;
        audio.play().catch(err => {
            console.warn('Close sound playback failed:', err);
        });
    }

    playNavSound() {
        if (!this.navInitialized || this.navSoundPool.length === 0) {
            return;
        }

        // Select random file
        const randomIndex = Math.floor(Math.random() * this.navSoundPool.length);
        const audio = this.navSoundPool[randomIndex];

        // Reset to start and play
        audio.currentTime = 0;
        audio.play().catch(err => {
            console.warn('Nav sound playback failed:', err);
        });
    }
}

// Create global instance
const soundManager = new SoundManager();

// Attach to all buttons (except navbar togglers) and nav links
document.addEventListener('click', function(e) {
    const buttonTarget = e.target.closest('button, a[role="button"]');
    if (buttonTarget && !buttonTarget.classList.contains('navbar-toggler')) {
        if (buttonTarget.classList.contains('btn-close')) {
            soundManager.playCloseSound();
        } else if (buttonTarget.classList.contains('btn')) {
            soundManager.play();
        }
        return;
    }

    const navTarget = e.target.closest('a.theme-nav-link');
    if (navTarget) {
        soundManager.playNavSound();
    }
});

