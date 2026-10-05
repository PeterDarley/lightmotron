# Theming

The UI uses a set of semantic CSS classes prefixed with `theme-` that cover every
visual element on the home page, plus standard Bootstrap classes for cards, modals,
buttons, and form controls that can be overridden globally.

## How to add a theme

1. Create `www/themes/my_theme.css`.
2. That's it. The device scans `www/themes/` automatically and lists every `.css` file
   it finds in the **Setup → Theme** picker. Select your theme there and click Apply.

Because Bootstrap is loaded first, then `app.css`, then your theme file, your rules win
without needing `!important` for most properties. The load order is managed by
`templates/base/imports.html` via a context variable injected on every page.

## Favicon (browser tab icon)

A theme can set its own tab icon. Put an SVG next to the theme's stylesheet, with the
same name:

- `www/themes/my_theme.css` + `www/themes/my_theme.svg` → the tab shows `my_theme.svg`.
- No matching `.svg` → the default icon, `www/favicon.svg`, is used.

The icon is chosen when a page is rendered (`favicon_href` in the context, set by
`components/web/context_processors.c`; linked in `templates/base/base_head.html`), so
switching themes changes the icon on the next page load. Browsers cache icons, so a
hard refresh may be needed to see a change.

Icons are SVG and are shipped with the firmware: they're in `www/themes/`, and the
theme upload form only accepts `.css` and font files. The built-in examples are:

| Theme | Icon | Description |
|---|---|---|
| `lcars.css` | `lcars.svg` | A copy of the default icon |
| `nautilus.css` | `nautilus.svg` | Ship's wheel: copper wheel, verdigris handles |

Keep icons simple. The tab shows them at about 16px, so fine detail won't be visible.

---

## Standard components

Pages use the same building blocks, so a theme only needs to style each one once.

| Element | Markup | Notes |
|---|---|---|
| Page title | `<h1>` | One per page |
| Card or panel | `.card` (or `.theme-home-section`) with `.card-header` and `.card-body` | Every section on the Status, Setup and Home pages |
| Card title | plain text inside `.card-header` | The header bar is the title; don't repeat it in the body |
| Subsection heading | `<h6 class="theme-section-heading">` | Use this for any heading inside a card or modal body |
| Modal | `.modal-content` with `.modal-header` (title in `.modal-title`), `.modal-body` | |
| Primary action | `.btn-primary` | One per section, such as Save or Manage |
| Secondary action | `.btn-outline-secondary` (`btn-sm` in lists) | |
| Destructive action | `.btn-outline-danger btn-sm` | Delete, stop, and similar |
| Final confirmation | `.btn-danger btn-sm` | Only on the confirmation step (Confirm Delete, Confirm Restore, Reboot) |
| Empty or summary text | `.text-muted small` | |

Don't create new heading or button styles for a single page. If a page needs something
new, add it as a shared class so every theme can style it.

---

## Class reference

All `theme-*` classes below have empty stubs in `app.css`. Bootstrap handles the default
appearance; your theme only needs to override what it changes.

### Page structure

| Class | Element | Notes |
|---|---|---|
| `theme-page` | Outer `<div>` wrapping the whole home page | Bootstrap `container` also applied |
| `theme-animation-panel` | Card containing the Start/Stop controls | Bootstrap `card` also applied |
| `theme-animation-label` | "Animation" text label inside the animation card | |
| `theme-animation-controls` | `<div>` wrapping Start and Stop buttons | |
| `theme-active-scene-panel` | Section containing the active-scene display | |
| `theme-active-scene-label` | "Active Scenes" label above the scene name | |
| `theme-scene-name` | The large scene name text | Default: 1.75 rem, weight 500 |
| `theme-scene-buttons` | Container for the Ongoing and Immediate sections | |
| `theme-ongoing-section` | Wrapper around the Ongoing scenes group | |
| `theme-immediate-section` | Wrapper around the Immediate scenes group | |
| `theme-section-heading` | `<h6>` labels "Ongoing" and "Immediate" | Default: small caps, muted |

### Buttons

| Class | Element | Notes |
|---|---|---|
| `theme-start-btn` | Start Animation button | Bootstrap `btn-success` / `btn-outline-success` also applied depending on state |
| `theme-stop-btn` | Stop Animation button | Bootstrap `btn-outline-danger` / `btn-danger` also applied depending on state |
| `theme-ongoing-btn` | Each ongoing scene toggle button | Bootstrap `btn-primary` (active) or `btn-outline-primary` (inactive) also applied |
| `theme-immediate-btn` | Each immediate scene trigger button | Bootstrap `btn-outline-warning` also applied |

### Navigation

| Class | Element | Notes |
|---|---|---|
| `theme-nav-link` | Every `<a>` in the navbar (brand + nav items) | Bootstrap `navbar-brand` or `nav-link` also applied |

### Form controls

| Class | Element | Notes |
|---|---|---|
| `theme-check` | Checkbox `<input>` | Bootstrap `form-check-input` also applied |
| `theme-check-label` | Checkbox `<label>` | Bootstrap `form-check-label` also applied |

### Effect editor controls

| Class | Element | Notes |
|---|---|---|
| `effect-optional-card` | Optional setting panel inside effect editor | Applied to each parameter block in `pattern_params.html` |
| `effect-param-slider` | Range input inside effect editor optional cards | Used by duration/frequency/period sliders |

Themes can override `--effect-slider-track`, `--effect-slider-track-focus`, and `--effect-slider-thumb` on `.effect-param-slider` to fine-tune slider contrast.

### Button state notes

Bootstrap state classes are added/removed dynamically by HTMX and `home.js`:

- `theme-ongoing-btn.btn-primary` — scene is currently active
- `theme-ongoing-btn.btn-outline-primary` — scene is inactive
- `theme-start-btn.btn-success` — animation is running
- `theme-start-btn.btn-outline-success` — animation is stopped
- `theme-stop-btn.btn-outline-danger` — animation is running
- `theme-stop-btn.btn-danger` — animation is stopped

To restyle a button for a specific state, combine the semantic class with the Bootstrap
state class:

```css
.theme-ongoing-btn.btn-primary {
    background-color: #8b1a1a;
    border-color: #8b1a1a;
}
```

---

## Global Bootstrap overrides

Themes can also restyle any Bootstrap component globally. Commonly overridden:

| Selector | What it affects |
|---|---|
| `body` | Page background and default text colour |
| `.navbar` | Top navigation bar |
| `.card`, `.card-header`, `.card-body`, `.card-footer` | All content cards |
| `.modal-content`, `.modal-header`, `.modal-body`, `.modal-footer` | All modals |
| `.btn-primary`, `.btn-secondary`, `.btn-danger`, etc. | All buttons of that Bootstrap variant |
| `.form-control`, `.form-select` | Text inputs and dropdowns |
| `.list-group-item` | List items |
| `.text-muted` | Muted helper text |
| `.alert-success`, `.alert-danger`, `.alert-warning` | Alert banners |
| `.status-card`, `.status-table` | Status-page card and table presentation |

> **Note:** Prefer overriding `theme-*` classes rather than Bootstrap classes directly where `theme-*` equivalents exist (e.g. use `.theme-check` instead of `.form-check-input`).

---

## Sound effects

Themes can define CSS custom properties to play sounds when users interact with the UI. Sound files must be placed in `www/sounds/` and referenced by filename only. Each variable takes a pipe-separated list of filenames; a random file from the list is played on each interaction.

| CSS variable | Triggered by |
|---|---|
| `--sound-files` | Any `.btn` button click |
| `--sound-files-close` | Any `.btn-close` button (modal/alert dismiss) |
| `--sound-files-nav` | Any `.theme-nav-link` navigation link click |

Example:
```css
:root {
    --sound-files: "click1.mp3|click2.mp3|click3.mp3";
    --sound-files-close: "cancel.mp3";
    --sound-files-nav: "navigate.mp3";
}
```

Omitting a variable disables sounds for that interaction. Sounds are pre-loaded on page load for instant playback.

---

## Example: dark red theme

The built-in `dark_red.css` (selectable in the Theme picker) is a minimal example:

```css
body { background-color: #1a0000; color: #f0c0c0; }

.navbar { background-color: #2a0000 !important; border-bottom: 1px solid #6a0000; }

.card { background-color: #240000; border-color: #6a0000; }
.card-header { background-color: #2e0000; border-bottom-color: #6a0000; color: #f0c0c0; }

.theme-animation-panel { background-color: #2a0000; border-color: #6a0000; }
.theme-animation-label, .theme-active-scene-label { color: #c07070 !important; }
.theme-scene-name { color: #ff6060; }
.theme-section-heading { color: #a04040 !important; }

.theme-ongoing-btn.btn-primary        { background-color: #8b0000; border-color: #8b0000; color: #fff; }
.theme-ongoing-btn.btn-outline-primary { border-color: #8b0000; color: #c07070; }
.theme-start-btn.btn-success          { background-color: #1f4a1f; border-color: #2d6a2d; color: #90ee90; }
.theme-start-btn.btn-outline-success  { border-color: #3a7a3a; color: #6ab46a; }
.theme-stop-btn.btn-danger            { background-color: #8b0000; border-color: #8b0000; }
.theme-stop-btn.btn-outline-danger    { border-color: #8b0000; color: #c07070; }
.theme-immediate-btn                  { border-color: #a06020; color: #c08030; }
```

## Example: Event Horizon theme

The built-in `event_horizon.css` is an ominous sci-fi style with deep black-maroon surfaces, crimson highlights, and toxic green accents:

It uses non-repeating atmospheric glow layers over a repeating grid texture, with a minimum page height of one viewport (`min-height: 100vh`).

```css
:root {
    --eh-blue: #cc4d65;
    --eh-green: #8abf36;
    --eh-purple: #8a3f5f;
}

body {
    background-color: #050711;
    color: #d6e3ff;
}

.theme-ongoing-section { border-left: 3px solid var(--eh-blue); }
.theme-immediate-section { border-left: 3px solid var(--eh-purple); }
.theme-sounds-section { border-left: 3px solid var(--eh-green); }

.theme-start-btn.btn-success { background-color: #4e6c21; border-color: #7fa83a; }
.theme-stop-btn.btn-danger { background-color: #681e2f; border-color: #9b3049; }
```
