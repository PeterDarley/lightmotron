# Theming the Home Page

The home page uses a set of semantic CSS classes prefixed with `home-` that cover every
visual element. You can create a custom CSS file, drop it into `www/styles/`, link it from
`templates/base/imports.html` **after** `app.css`, and override only the classes you care
about. No template changes are required.

## How to add a theme

1. Create `www/styles/my_theme.css`.
2. Open `templates/base/imports.html` and add a `<link>` tag after the existing app CSS line:
   ```html
   <link href="/styles/app.css" rel="stylesheet">
   <link href="/styles/my_theme.css" rel="stylesheet">
   ```
3. Add rules for whichever `home-*` classes you want to change (see reference below).

Because Bootstrap is loaded first, then `app.css`, then your theme, your theme rules win
without needing `!important` for most properties.

---

## Class reference

All classes below are present on the live page. Empty stubs in `app.css` mean Bootstrap
handles the default appearance; your theme only needs to override what it changes.

### Page structure

| Class | Element | Notes |
|---|---|---|
| `home-page` | Outer `<div>` wrapping the whole page | Bootstrap `container` is also applied |
| `home-animation-panel` | Card containing the Start/Stop controls | Bootstrap `card` is also applied |
| `home-animation-label` | "Animation" text label inside the card | |
| `home-animation-controls` | `<div>` wrapping Start and Stop buttons | |
| `home-active-scene-panel` | Section containing the active-scene display | |
| `home-active-scene-label` | "Active Scenes" label above the scene name | |
| `home-scene-name` | The large scene name text | Default: 1.75 rem, weight 500 |
| `home-scene-buttons` | Container for the Ongoing and Immediate sections | |
| `home-ongoing-section` | Wrapper around the Ongoing scenes group | |
| `home-immediate-section` | Wrapper around the Immediate scenes group | |
| `home-section-heading` | `<h6>` labels "Ongoing" and "Immediate" | Default: small caps, muted |

### Buttons

| Class | Element | Notes |
|---|---|---|
| `home-start-btn` | Start Animation button | Bootstrap `btn-success` / `btn-outline-success` also applied depending on state |
| `home-stop-btn` | Stop Animation button | Bootstrap `btn-outline-danger` / `btn-danger` also applied depending on state |
| `home-ongoing-btn` | Each ongoing scene toggle button | Bootstrap `btn-primary` (active) or `btn-outline-primary` (inactive) also applied |
| `home-immediate-btn` | Each immediate scene trigger button | Bootstrap `btn-outline-warning` also applied |

### Button state notes

Bootstrap state classes are added/removed dynamically by HTMX and `home.js`:

- `home-ongoing-btn.btn-primary` — scene is currently active
- `home-ongoing-btn.btn-outline-primary` — scene is inactive
- `home-start-btn.btn-success` — animation is running
- `home-start-btn.btn-outline-success` — animation is stopped
- `home-stop-btn.btn-outline-danger` — animation is running
- `home-stop-btn.btn-danger` — animation is stopped

To restyle buttons for a specific state, combine the semantic class with the Bootstrap state
class, e.g.:

```css
.home-ongoing-btn.btn-primary {
    background-color: #8b1a1a;
    border-color: #8b1a1a;
}
```

---

## Example: dark red theme

```css
/* www/styles/dark_red.css */

body { background-color: #1a0000; color: #f0c0c0; }

.home-animation-panel { background-color: #2a0000; border-color: #6a0000; }
.home-animation-label, .home-active-scene-label { color: #c07070 !important; }
.home-scene-name { color: #ff6060; }
.home-section-heading { color: #a04040 !important; }

.home-ongoing-btn.btn-primary        { background-color: #8b0000; border-color: #8b0000; color: #fff; }
.home-ongoing-btn.btn-outline-primary { border-color: #8b0000; color: #c07070; }
.home-start-btn.btn-outline-success  { border-color: #4a7a4a; color: #4a7a4a; }
.home-start-btn.btn-success          { background-color: #2d5a2d; border-color: #2d5a2d; }
.home-stop-btn.btn-danger            { background-color: #8b0000; border-color: #8b0000; }
.home-stop-btn.btn-outline-danger    { border-color: #8b0000; color: #c07070; }
.home-immediate-btn                  { border-color: #a06020; color: #c08030; }
```
