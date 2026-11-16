# Writing a screen as data

A screen is three JSON files in `lib/ui_decl/screens/`. LVGL draws them the same way on the Mac, in the browser and (once it's wired in) on the board.

| File | What it holds | Who changes it |
|---|---|---|
| `theme.json` | Design tokens: colours, type scale, spacing, radii, each widget type's defaults, and named styles | Design, rarely |
| `<screen>.json` | The widget tree and its layout | Design |
| `<screen>.data.json` | Sample data for the preview. On the board, the app supplies the same shape. | Whoever writes the app |

Comments starting with `//` are allowed in all three files.

## Preview it

```
make ui-decl-serve        # builds the WebAssembly preview, serves the repo on 127.0.0.1:8089
```

Open `http://localhost:8089/build/ui-decl-web/index.html`. Edit any of the three files and save. The page checks them every second and redraws on its own. Errors show in red above the screen. A layout problem (a label cut off by its box or by the screen) is listed there too.

`make ui-shots` renders the same screens to `build/ui-shots/decl/*.png` and fails on layout problems. `make ui-decl-compare` checks that the browser's pixels match the Mac's exactly.

The first build needs Emscripten: `brew install emscripten`.

## The tree

Every node is an object with a `"type"`. A screen file has one `"root"` node:

```json
{ "root": { "type": "page", "style": "screen", "width": "100%", "height": "100%",
            "layout": { "flex": "column", "gap": "$space.sm" },
            "children": [ { "type": "label", "text": "{synth.name}" } ] } }
```

| type | What it is | Its own keys |
|---|---|---|
| `page` | A box that holds other nodes. It has no styling of its own, so it's invisible until you style it. | `layout`, `children` |
| `label` | Text | `text`, `overflow` (`wrap`, `dots`, `clip`, `scroll`) |
| `bar` | A value bar, full width by default | `min`, `max`, `value` |
| `knob` | A 270° arc, like a pot's travel | `min`, `max`, `value`, `size` |
| `button` | A button with a label | `text` |
| `list` | A scrolling list | `items` (an array, or a binding to one) |

Any node can also take:
- `id`, so the app can find it later;
- `width` and `height` (px, `"50%"` or `"content"`), or `size` for both;
- `grow`, a flex share;
- `cell`, its grid position;
- `style` and `style_when`;
- `hidden`;
- `for`;
- `note`, which is ignored, so you can leave a comment there.

A misspelt key is an error, not something silently ignored.

## Layout

LVGL's flexbox and grid, named the way CSS names them:

```json
"layout": { "flex": "row", "main": "space_between", "cross": "center", "gap": 8 }
"layout": { "grid": { "cols": ["1fr", "1fr", "1fr", "1fr"], "rows": ["1fr", "1fr"] }, "gap": "$space.sm" }
"cell":   { "col": 2, "row": 1, "col_span": 1, "row_span": 1, "x": "stretch", "y": "center" }
```

- `flex` is one of `row`, `column`, `row_wrap` or `column_wrap`.
- `main` and `cross` take `start`, `center`, `end`, `space_between`, `space_around` or `space_evenly`.
- A grid track is a number of pixels, `"content"`, or `"<n>fr"`.

## Tokens and styles

Anywhere a value goes, you can name a token instead: `"$color.accent"`, `"$type.display"`, `"$space.md"`. A style is either a theme style's name (`"style": "card"`) or an object of properties (`"style": { "use": "card", "pad": 4 }`). The `use` key starts the object from a named style.

Style keys:
- colour and opacity: `bg`, `bg_opa`, `text`, `opa`;
- type: `font`, `text_align`, `letter_space`;
- spacing: `pad`, `pad_x`, `pad_y`, `pad_top`, `pad_bottom`;
- shape: `radius`, `border`, `border_color`;
- bars and knobs: `accent` (the fill), `track` (what the fill sits on), `arc_width`.

Colours are written `"#RRGGBB"`.

Each widget starts from the theme's `defaults` for its type, then gets its own `style`.

Font sizes snap down to the sizes the firmware compiles in: Montserrat 10, 12, 14, 16, 18, 20, 22, 24 and 28. Asking for 40 gets 28. A larger size means adding it to `firmware/config/lv_conf.h`, which makes the firmware bigger.

## Data

Text in braces comes from the data file: `"text": "{page.name}  {page.index}/{page.count}"`. A value that is only a binding (`"value": "{param.value}"`) keeps its type, so numbers stay numbers.

`"for": "page.knobs"` repeats a node once for each item in that array. Inside the repeated node, a binding looks in the current item first, then in the whole data file. An `id` on a repeated node gets the item's index on the end (`knob.0` to `knob.7`).

`"style_when": { "{active}": "knob_on" }` adds the `knob_on` style when `active` is true.

## Limits today

- **Not on the board yet.** This was rendered on the Mac (`make ui-shots`) and in the browser only. Getting the same pixels on the P4 needs the board's own display driver and a screenshot over the diag page (milestone E1).
- **A small widget set.** There are no images or icons, no sliders, no animations, no touch handlers and no live updates. The app updates nodes through their ids for now.
- **LVGL's own XML format wasn't evaluated.** That format and its editor were not tried tonight, and their licence wasn't checked.
