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
