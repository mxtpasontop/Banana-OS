# Banana OS themes

A theme sets the colours of the desktop: windows, the taskbar, the Start
menu, menus and panels, and the apps that come with Banana OS (Files,
Settings, Task Manager, Notepad...). It can bring a wallpaper too.

Themes are small text files. Make one on Banana OS itself or on Linux, check
it, look at it, and install it.

## The file

`ocean.theme`:

```ini
# lines starting with # are comments (on lines of their own)
name=Ocean
author=Banana OS
# optional: a built-in wallpaper's name, or a picture packed with the theme
wallpaper=Ocean Wave
shadow=#000000
base=#0B1A20
surface=#10262E
raised=#1C3A44
line=#2F5A66
text_dim=#8FB3BD
text=#E3F4F7
bright=#FFFFFF
accent=#2EC4B6
```

A theme also picks **Task Manager's look**:

```ini
# banana, classic, windows10 or windows11 (default: windows10)
taskmgr=windows11
```

| `taskmgr=` | Task Manager |
|---|---|
| `banana` | Banana OS's own first one: tabs, a list, CPU and memory bars |
| `classic` | Windows XP / 7: grey, Applications, Processes, Performance with green meters, Networking, Users |
| `windows10` | Windows 10: a menu bar and tabs, light, the busy columns tinted yellow |
| `windows11` | Windows 11: a rail of pages at the left, dark |

Settings > Themes and
`theme taskmgr STYLE` can choose another look whatever the theme says.

Colours are `#RRGGBB` (or `RRGGBB`, `0xRRGGBB`). `name`, the six shades
`base` to `text`, and `accent` are required; `shadow`, `bright`, `author`
and `wallpaper` may be left out.

| Key | What it paints |
|---|---|
| `shadow` | the darkest edges and shadows (default: black for a dark theme, a grey for a light one) |
| `base` | the deepest backgrounds: lists, the taskbar, a window's sunken parts |
| `surface` | windows' and panels' backgrounds |
| `raised` | buttons, title bars, tabs, what stands out of a surface |
| `line` | borders, separators, frames of graphs |
| `text_dim` | secondary text: hints, labels, greyed items |
| `text` | text |
| `bright` | the brightest text and highlights (default: white for a dark theme, black for a light one) |
| `accent` | the colour of selection, focus, links, the chosen item, progress |

A **light** theme is one whose `base` is lighter than its `text`: the whole
interface turns over (dark text on light surfaces).

### How the desktop's colours become the theme's

Banana OS draws its interface with one palette (the theme *Banana Dark*).
With another theme, each of those colours is replaced:

* a **grey** (or a greyish blue) goes by its lightness: it falls between two
  of Banana Dark's shades - say between `surface` and `raised` - and becomes
  the colour the same way between your `surface` and `raised`;
* a **blue** (the accent family) becomes your `accent`, as much lighter or
  darker as it was than Banana Dark's accent;
* other colours - red (close, errors), green (OK), yellow (focus), orange
  (LIVE CD) - stay what they are, and so do icons, pictures, the Start orb
  and what terminals print.

Windows drawn light by design (Files, the Start menu) are mapped the other
way round: their white is your `base`, their dark text your `text` - so a
dark theme makes them dark too.

Keep `text` far from `base` and `surface` in lightness (120 and more out of
255 reads well); `bananatheme check` and `theme check` warn when it is not.

## On Banana OS

* **Settings > Themes**: every theme, drawn small in its own colours; a click
  uses it. *Install theme...* takes a `.theme` or `.btheme` file; *Save a
  copy* writes the theme in use to `~/Documents/NAME.theme` to change.
* In a terminal:

```
theme                       the themes (* the one in use)
theme set Ocean             use one
theme show [NAME]           its colours
theme new Mine              ~/Mine.theme: a copy of the theme in use, to edit (Notepad)
theme check Mine.theme      errors and readability warnings
theme install Mine.theme    install (into /usr/share/themes), then: theme set Mine
```

## On Linux: `sdk/tools/bananatheme`

Python 3 and nothing else.

```bash
sdk/tools/bananatheme new "My Theme"            # my-theme.theme, from Banana Dark
sdk/tools/bananatheme new "Paper" --light       # from the Light theme
sdk/tools/bananatheme check my-theme.theme
sdk/tools/bananatheme preview my-theme.theme    # my-theme.png: a window, Files and the taskbar in it
sdk/tools/bananatheme pack my-theme.theme --wallpaper beach.jpg   # my-theme.btheme
```

`preview` works the colours out exactly as Banana OS does, so the picture
shows what you will get. A **`.btheme`** is a ZIP holding the `.theme` and
its picture (its `wallpaper=` names the picture); Banana OS installs both.

Copy the file to Banana OS - a USB stick, the Browser, `curl`, a shared
folder - and install it from Settings > Themes or with `theme install`.

## Examples

`examples/lavender.theme` (light) and `examples/neon.theme` (dark).
The built-in themes: Banana Dark, Light, Midnight, Ocean, Forest, Banana,
Sakura and High Contrast (`theme show NAME` prints their colours).
