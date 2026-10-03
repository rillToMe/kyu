# KyuzenOS UI Rules

Part of the KyuzenOS Development Rules. See `RULES.md` for general principles.

Version: 1.0

These rules are **binding**. A UI change that violates them is a bug, even if it
compiles and looks acceptable on screen. Rules are numbered (`UI-n.m`) so a
review comment can cite one instead of arguing about taste.

This file is the normative reference. The how-to companion is
[`docs/design/gui/libui-design-system.md`](../docs/design/gui/libui-design-system.md);
where the two disagree, this file wins.

---

# 0. Scope

UI-0.1. These rules apply to everything that paints KyuzenOS userspace UI:

| Path | What it is |
| --- | --- |
| `libs/gui/widget/` | the toolkit (`libui`), including `theme/`, `core/`, widgets, XML |
| `include/libui.h`, `include/libui_xml.h` | the public C ABI |
| `apps/` | applications built on the toolkit |
| `system/desktop/` | the desktop shell (see UI-13.3, it has a narrow exemption) |
| `ui/xml/` | declarative UI documents |

UI-0.2. The design system is owned centrally by `libs/gui/widget/`. If a visual
decision has to be made, it is made **there** — never in an application, and
never twice.

UI-0.3. When a visual problem can be fixed in the design system or a widget,
fix it there. Patching one application's drawing code to look right is a rule
violation, not a fix (see `RULES.md` §2, "reuse existing utilities").

---

# 1. Visual Language

The language is built from four things, in priority order.

UI-1.1. **Typography carries hierarchy.** Title, body, and caption are
distinguished by size, tracking, case, and tone — not by colored boxes.

UI-1.2. **Space separates; lines delimit.** Gaps are the primary separator. A
1px rule is for separating *sections*, not every element.

UI-1.3. **Depth is information.** Shadow means "this surface floats above other
content". Content that sits in the page flow is never shadowed.

UI-1.4. **Accent carries meaning.** Accent marks the primary action, the
selected value, and focus. It is not decoration, and it is not the product
identity — the default accent is `NEUTRAL`, not blue.

UI-1.5. **Restraint.** Every visible element must communicate information or
afford interaction. Anything that does neither is removed, not tuned.

UI-1.6. **Dense but breathable.** Do not manufacture empty space to look
modern. Desktop UI is allowed to be information-dense.

UI-1.7. Modernity MUST come from hierarchy, spacing, alignment, and
interaction quality. It MUST NOT come from increasing `border-radius`,
`shadow`, `gradient`, `padding`, or animation. See §10 for the hard list.

---

# 2. Token Ownership

UI-2.1. Every visual value MUST come from a token in
`libs/gui/widget/include/theme/` or a field of `ui::Theme`. Widgets and
applications MUST NOT contain literal visual values.

```cpp
// Correct
p.surface(x, y, w, h, p.theme.surface_elevated, p.theme.metrics.radius_control);
p.text_role(label, x, y, p.theme.tone(TONE_SECONDARY), p.theme.type.caption);

// Violation
p.rrect(x, y, w, h, 8, color_hex(0x2C5EF5));
int padding = 13;
```

UI-2.2. Tokens live in exactly one place:

| File | Owns |
| --- | --- |
| `theme/colors.hpp` | base neutrals, accent base, derivation ratios, `TextTone` |
| `theme/typography.hpp` | `TypeRole`, the `Typography` table, text measurement |
| `theme/metrics.hpp` | spacing, radius, control/chrome/list/scrollbar/icon metrics |
| `theme/elevation.hpp` | `Elevation` levels and the shadow policy |
| `theme/motion.hpp` | durations, easing, frame budget |
| `theme/icons.hpp` | the `Icon` enum and icon geometry |
| `core/theme.hpp` | the resolved `Theme` struct widgets actually read |

UI-2.3. If a value you need is not in the token set, **add the token** (with a
reason) — do not inline the value. A missing token is a gap in the design
system, and closing it benefits every widget.

UI-2.4. A widget MUST NOT read the theme mode or accent to decide how to draw.
It reads resolved tokens. `Theme::surface_for(level)` maps elevation to a
surface; `Theme::tone(TextTone)` maps a semantic tone to a color. Use those
instead of branching on `mode == UI_THEME_DARK`.

UI-2.5. Token values are frozen for the legacy path. The `ui_theme_t`
6-color route must stay **pixel-identical** (locked by
`tests/host/unit/libui_theme_test.cpp`). Changing a base neutral or an accent
base requires updating the tests in the same commit and documenting why.

---

# 3. Spacing

UI-3.1. One scale, no arbitrary values:

```text
xs=4   sm=8   md=12   lg=16   xl=24   xxl=32   xxxl=48
```

UI-3.2. Use the token, not the number:

```c
ui_vbox_create(win, UI_SPACE_LG);   // correct
ui_vbox_create(win, 13);            // violation
```

UI-3.3. Values off the scale (`7`, `11`, `13`, `19`, `27`) are a violation
unless a comment on the line documents the derivation. The established
derivations are: control heights from a 16px glyph plus vertical breath, and
chrome bar heights chosen so a bar reads as a band rather than a text row.

UI-3.4. In XML, `spacing` and `<grid padding>` MUST use the tokens
(`xs`/`sm`/`md`/`lg`/`xl`) or an off-scale px value justified per UI-3.3. The
inflater accepts any integer in range; that permissiveness is **not** an
invitation (see UI-7.5).

---

# 4. Corner Radius

UI-4.1. A small set of meaningful levels. Do not invent a radius per widget.

| Token | Value | Use for |
| --- | --- | --- |
| `radius::NONE` | 0 | bars, tables, editors |
| `radius::SMALL` | 3 | checkboxes, tracks |
| `radius::CONTROL` | 5 | buttons, inputs, dropdowns |
| `radius::CONTAINER` | 7 | menus, popups, panels |
| `radius::DIALOG` | 9 | modals |
| `radius::PILL` | clamped to half height | only shapes that genuinely are capsules: switch, slider grip, radio ring |

UI-4.2. `PILL` MUST NOT be used to make ordinary controls (buttons, inputs,
tabs, cards) look softer. Pill-shaped controls everywhere is a listed
anti-pattern (UI-10.1).

---

# 5. Control Metrics

UI-5.1. Control heights come from tokens, not from per-widget choice:

| Token | Value | Use for |
| --- | --- | --- |
| `control::H_SM` | 24 | dense rows, in-list controls |
| `control::H_MD` | 28 | **default** — buttons, inputs, dropdowns |
| `control::H_LG` | 36 | the primary action of a page |

UI-5.2. A form control MUST NOT pick its own height. If a control needs a
different height, the token set is missing an entry (UI-2.3).

UI-5.3. All controls that appear side by side in one row MUST share a height,
or be explicitly aligned to a baseline. Mixing 28px and 24px controls in one
row is a violation.

---

# 6. Typography

UI-6.1. Text MUST be drawn through a `TypeRole`, never with an ad-hoc size:

```cpp
p.text_role(title, x, y, p.theme.tone_for(p.theme.type.title, enabled),
            p.theme.type.title);
```

UI-6.2. Roles are fixed. Do not add a role for one screen:

| Role | Line height | Tracking | Uppercase | Default tone |
| --- | --- | --- | --- | --- |
| `display` | 32 | 1 | no | primary |
| `title` | 24 | 1 | no | primary |
| `section` | 20 | 0 | yes | secondary |
| `body` | 20 | 0 | no | primary |
| `body_emphasis` | 20 | 0 | no | primary |
| `label` | 18 | 0 | no | primary |
| `caption` | 16 | 0 | no | secondary |
| `mono` | 16 | 0 | no | primary |

UI-6.3. Hierarchy MUST be achievable with the roles above. If a screen needs a
ninth visual weight, the screen's structure is wrong, not the type scale.

UI-6.4. Text color MUST come from `theme.tone()` / `theme.tone_for()`. Choosing
`text` vs `text_secondary` by hand per widget is how hierarchy drifts.

UI-6.5. `theme.tone_for(role, enabled)` is the single path for the disabled
case: **disabled always wins over the role's tone.**

UI-6.6. Text that can overflow its box MUST be drawn with `text_ellipsis()`.
Text MUST NOT be allowed to paint outside widget bounds — damage tracking
depends on widgets staying inside their rects.

UI-6.7. The renderer reality must be respected. The toolkit's own path is a
bitmap 8x16 font with a fixed 8px advance; `ft_size_px`/`ft_weight` on
`TypeRole` are *declared intent* for applications that render through
FreeType. Do not assume per-role pixel sizes are applied by the toolkit.

---

# 7. Fonts

UI-7.1. An application that wants the user's chosen UI font MUST call
`ui::uifont_install()` **before creating its first widget**:

```cpp
#include "services/uifont.hpp"

int MyApp::run() {
    ui::uifont_install();   // first, before any widget exists
    ...
}
```

UI-7.2. Why the ordering is mandatory: button widths, labels, and list row
heights are measured from font metrics **at widget construction time**.
Installing the provider afterwards leaves every size wrong. There is no
supported way to fix it up later.

UI-7.3. The toolkit core deliberately does not link FreeType. Applications that
opt in add the separate objects:

```cmake
LIBS kyuzen-widget-uifont kyuzen-text-manager kyuzen-text-raster kyuzen-freetype
```

UI-7.4. Do not add FreeType to the toolkit core to "make typography nicer".
That decision is deliberate and is what keeps the core asset-free.

UI-7.5. `TextProvider` MUST be either complete or absent. A provider missing
`measure`, `line_height`, or `draw` is treated as "no provider" and the toolkit
falls back to the bitmap font. A half-installed provider that draws part of the
UI in a real font and part in bitmap is a violation.

---

# 8. Text Rendering Correctness

These rules exist because breaking them produces a real, shipped bug: text
that visibly thickens every time a region is repainted.

UI-8.1. **Text compositing MUST be idempotent.** Drawing the same string twice
onto the same pixels MUST produce the same result as drawing it once. A
coverage-blended glyph drawn over its own output accumulates ink at the
antialiased edges; solid interior pixels are unaffected, which is why the bug
looks like "the text got bolder" rather than "the text is wrong".

UI-8.2. A text provider's `draw` MUST composite coverage over an explicit
background color, not over whatever is currently in the canvas. The `bg`
argument in `ui_text_draw_fn` exists for exactly this reason:

```c
typedef void (*ui_text_draw_fn)(void* ud, uint32_t* canvas, int cw, int ch,
                                int x, int baseline_y, color_t fg, color_t bg,
                                const char* text);
```

UI-8.3. In `libs/text/`, use `kz_text_draw_on()` (idempotent) for anything that
can be repainted. `kz_text_draw()` accumulates and MUST only be used where the
destination is guaranteed fresh.

UI-8.4. A widget that paints its own background MUST declare that background to
the painter with `Painter::surface_rect()` (or `set_text_bg()`), so text drawn
on top composites against the correct color. A widget that draws `rect()`
instead of `surface_rect()` and then draws text is a violation.

UI-8.5. A widget MUST NOT draw its children twice per frame. If a container
needs its child laid out before it can compute its own size, it calls
`settle()` (arrange only) — it does not perform a throwaway draw pass. A draw
pass used as a layout pass is what turned UI-8.1 into a user-visible defect.

UI-8.6. Partial repaints are normal and MUST be safe. Hover, focus, and damage
rects repaint subsets of the tree; any widget whose output depends on how many
times it has been drawn violates UI-8.1.

UI-8.7. Regression lock: `tests/host/unit/text_test.c` asserts that
`kz_text_draw_on()` is byte-identical after repeated passes **and** that
`kz_text_draw()` is not. Do not weaken that assertion to make a change pass.

---

# 9. Widget State Model

UI-9.1. Interactive widgets MUST define every state that applies to them:

```text
normal  hover  pressed  focused  disabled  selected  checked  invalid
```

Not every widget needs all of them, but each applicable state must be visually
**and** behaviorally defined. A control with a hover state but no visible focus
state is incomplete.

UI-9.2. State MUST be mapped through `core/state.hpp`, not through a per-widget
if/else color chain:

```cpp
ui::StateInputs st;
st.hover = hover; st.pressed = pressed; st.focused = has_focus;
st.enabled = enabled; st.invalid = invalid;
ui::SurfaceStyle s = ui::state_style(p.theme, st);
```

UI-9.3. Priority is fixed and has no exceptions:

```text
disabled  >  pressed  >  hover
focus     affects the BORDER only, never the fill
selected  affects the FILL (list rows), never the border
```

UI-9.4. **Focus MUST be visible even while the pointer is over the widget.**
`state_border()` already encodes this; do not reorder it.

UI-9.5. **Disabled always wins.** A disabled control that still shows a hover
highlight is a violation.

UI-9.6. Invalid/error state is signalled by `danger` on the border, and for text
fields by an accompanying message. It MUST NOT be signalled by a red fill.

UI-9.7. Focus rings and focus underlines are drawn **inside** the widget bounds
(`focus::RING_W`, `focus::OUTLINE_W`). Drawing outside the bounds breaks damage
tracking.

UI-9.8. Every interactive control MUST be reachable and operable by keyboard.
Tab order follows visual order. Arrow keys move within a composite control
(list, radio group, tabs) and MUST be consumed by it rather than leaking to the
parent container.

UI-9.9. A widget that takes keyboard focus but has nothing to do with arrow keys
MUST opt out via `ui_widget_set_focusable()` / `focus_opt_out`, so it does not
swallow keys meant for an ancestor.

---

# 10. Anti-Patterns (Hard Prohibitions)

UI-10.1. The following MUST NOT be introduced:

```text
card-inside-card layouts                    decorative blobs
excessive rounded cards                     oversized hero sections
glassmorphism                               giant typography with no function
gradient backgrounds                        pill controls everywhere
excessive drop shadows                      SaaS dashboard layouts
decorative glow                             random blue accents
excessive borders                           emoji as UI icons
excessive transparency                      mixed icon styles
excessive animation                         unnecessary badges
arbitrary spacing/colors/radii              visual noise
```

UI-10.2. Shadows are permitted **only** on `ELEV_POPUP` (4 rings) and
`ELEV_DIALOG` (6 rings). `ELEV_BASE`, `ELEV_SURFACE`, and `ELEV_RAISED` have
explicitly zero shadow. Page content — cards, list rows, buttons, inputs — is
never shadowed.

UI-10.3. Never stack effects. Shadow + gradient + glow on one element is a
violation regardless of how subtle each is individually.

UI-10.4. `Painter::vgrad()` and `Painter::rrect_grad()` exist for the legacy
button rendering path. New UI MUST NOT use a gradient. If you believe a gradient
is required, that is a design-system proposal, not a widget change.

UI-10.5. Depth MUST be the minimum treatment that communicates the relationship.
Prefer a surface color change over a border, and a border over a shadow.

UI-10.6. A container whose only purpose is to hold one child with a rounded
background and a shadow is a card-in-card. Remove it.

---

# 11. Icons

UI-11.1. Icons are referenced by **semantic name**, never drawn ad hoc:

```c
ui_button_set_icon(btn, UI_ICON_SETTINGS);
ui_listview_set_row_icon(lv, 0, UI_ICON_DISPLAY);
```

UI-11.2. `ui::Icon` in `theme/icons.hpp` and `UI_ICON_*` in `include/libui.h`
MUST stay in the same order — the ABI value is used directly as the enum. The
`static_assert` locking this must not be removed.

UI-11.3. Adding an icon means adding a case to the semantic enum and its
geometry in one place. An application MUST NOT draw its own glyph, triangle,
`+`/`-`, or box.

UI-11.4. Icon optical size comes from `icon::SM|MD|LG|XL` via
`ui_icon_size(UI_ICON_SIZE_*)`. A hand-picked icon size is a violation.

UI-11.5. Emoji are not UI icons. Unicode box-drawing and symbol characters are
not UI icons. Both render inconsistently and neither matches the stroke grid.

UI-11.6. The icon style is fixed: 1px stroke, square corners, no fill, no free
curves. Do not add a filled or rounded icon variant.

---

# 12. Layout

UI-12.1. Applications MUST use the layout system (VBox/HBox/Grid/ScrollView)
rather than computing child positions by hand. Manual pixel arithmetic in an
application is a violation.

UI-12.2. Sizing MUST be content-driven where the content is known: let the
layout compute it. Hardcoding a width that happens to fit today's string is a
violation.

UI-12.3. `width`/`height` in XML are for genuine intent (a viewport, a preview
box, a fixed sidebar). They MUST NOT be used to work around a layout that
should have sized itself.

UI-12.4. The toolkit's minimum target is **1280×720**. Layouts MUST work at
that size; they MUST NOT assume more room. Layouts SHOULD still be sane at
1920×1080.

UI-12.5. Content that can exceed the viewport MUST be inside a `ScrollView`.
Clipping content without a way to reach it is a violation.

UI-12.6. Pixel alignment: prefer even offsets for text and 1px borders. The
shared helpers (`ui::snap`, `ui::clamp_int`) exist so widgets do not each
re-derive alignment. Blurry 1px borders and half-pixel text are review
findings, not cosmetic preferences.

---

# 13. Architecture & Layering

UI-13.1. The dependency direction is fixed:

```text
core  →  primitives / layout  →  containers / chrome / dialog  →  window  →  abi
```

UI-13.2. A lower layer MUST NOT include `window/window.hpp`. Declare
`class Window;` instead. Only the `.cpp` files that genuinely call `Window`
methods include it. This is a hard invariant of the split.

UI-13.3. Applications MUST NOT talk to kernel graphics internals to achieve a
visual effect. `libui` → rendering backend → graphics syscalls is the only path.
`system/desktop/` is the one documented exception: it draws directly through
`libgui` because it owns the composited desktop window. That exemption does not
extend to applications, and it does not license new direct-drawing code in the
shell.

UI-13.4. `operator new` / `operator delete` and `__cxa_pure_virtual` are defined
**once**, in `libs/gui/widget/src/runtime/runtime.cpp`. Defining them anywhere
else is an ODR violation.

UI-13.5. There MUST be no global or static object with a non-trivial
constructor. The ELF loader does not run `.init_array`. Use function-local
`const` POD tables instead.

UI-13.6. All math in the toolkit is integer-only. Every target build inherits
`KYUZEN_FREESTANDING_BASE` (`cmake/KyuzenCompiler.cmake`), which includes
`-mno-sse -mno-sse2 -mno-mmx -msoft-float`. These are **correctness** flags, not
tuning: the kernel never sets `CR4.OSFXSR`, so any SSE instruction faults. Do
not introduce `float`/`double`, and do not introduce a runtime dependency that
requires them. `cmake/verify_user_elf.cmake` checks the resulting ELF.

UI-13.7. `libui.h` and `libui_xml.h` are **append-only** and use opaque handles.
Do not reorder enums, change existing struct layouts, or change existing
function signatures. Add new functions instead.

UI-13.8. Do not break existing applications to modernize the UI. Preserve the
existing API, add compatibility where practical, and deprecate gradually.
If an API is genuinely unfixable, document the migration path **before**
removing it.

UI-13.9. Do not overengineer. The following MUST NOT be introduced into `libui`:

```text
dependency injection framework     virtual DOM
reactive framework                 CSS engine
JavaScript runtime                 component registry
reflection system                  template metaprogramming layer
```

`libui` is a native C++ widget toolkit. Keep it understandable.

UI-13.10. Avoid giant widget classes and duplicated rendering code. If two
widgets draw the same thing, that drawing belongs in `Painter` or a shared
helper. If a widget's `draw()` no longer fits on a screen, the widget is doing
too much.

UI-13.11. Widgets MUST be composed rather than subclassed into hierarchies.
A settings row is a composition of existing primitives, not a new widget class.

---

# 14. Lists & Forms

UI-14.1. A list row has one canonical shape:

```text
[icon]  title                       [trailing]  [chevron]
        description
```

```c
int row = ui_listview_add_row(lv, "Display",
                              "Resolution, scaling, night light",
                              UI_ICON_DISPLAY, /*chevron=*/1);
```

UI-14.2. A row with a `description` becomes two rows high automatically. Do not
hand-build two-line rows.

UI-14.3. Rows MUST support hover, selection, focus, and disabled. A row that
cannot be disabled cannot represent unavailable state.

UI-14.4. **Do not wrap each row in its own rounded card.** The selected row uses
the `selection` ink; hover uses `surface_hover`. This is the single most common
way an interface acquires card-in-card structure (UI-10.1).

UI-14.5. Settings pages are built from lists plus `<section>`, not from cards:

```c
ui_widget_t* sec = ui_section_create(win, "Appearance", UI_SPACE_SM);
ui_layout_add(sec, row1);
ui_layout_add(sec, row2);
```

UI-14.6. Form controls MUST share typography, height, spacing, focus
treatment, border treatment, disabled behavior, and validation behavior. A text
field that focuses differently from a dropdown is a violation.

UI-14.7. Do not add floating-label behavior. It is not part of this language.

---

# 15. Choosing a Control

UI-15.1. Pick the control that matches the interaction, not the one that looks
best in isolation:

| Need | Control | Why |
| --- | --- | --- |
| primary action | `button` + `variant="primary"` | exactly one per surface |
| ordinary action | `button` (secondary) | the default |
| quiet action (Cancel, Skip) | `button` + `variant="tertiary"` | no fill or border — prevents a wall of boxes |
| destructive action | `button` + `variant="danger"` | |
| icon-only action | `button` + `icon`, empty text | becomes square automatically |
| value submitted with a Save | `checkbox` | part of a form |
| setting that applies immediately | `switch` | on/off without a Save |
| exclusive choice | `radio` + `group` | |
| short fixed list | `combobox` | |
| a range | `slider` | |

UI-15.2. A surface MUST NOT have more than one primary button. If two actions
both look primary, one of them is not.

UI-15.3. Use `tertiary` for the escape action. Two adjacent filled buttons for
"OK" and "Cancel" is a hierarchy failure.

---

# 16. Elevation

UI-16.1. Depth levels are fixed:

| Level | Surface | Shadow |
| --- | --- | --- |
| `ELEV_BASE` | `bg` | none |
| `ELEV_SURFACE` | `surface` | none |
| `ELEV_RAISED` | `surface_elevated` | none |
| `ELEV_POPUP` | `panel` | yes, 4 rings |
| `ELEV_DIALOG` | `panel` | yes, 6 rings |

UI-16.2. Read the surface with `theme.surface_for(level)` rather than picking a
color. This is what keeps "sometimes `surface`, sometimes `surface_elevated`"
from re-appearing.

UI-16.3. `surface_variant` is a **recessed** surface (wells: inputs, tracks,
gutters) and always moves toward `bg`. `surface_elevated` is a **raised**
surface and moves toward the text. They are guaranteed distinct in both modes
and locked by test. Do not collapse them.

UI-16.4. A widget that floats MUST ask for damage margin covering its shadow
(`elevation_shadow_margin`). Otherwise the shadow leaves residue when the
surface moves or disappears.

---

# 17. Motion

UI-17.1. Durations are tokens: `instant=0`, `fast=90`, `normal=130`, `slow=180`.

UI-17.2. Hover and press MUST NOT be animated. They must feel instant, and they
change small rects — adding frames there adds rendering cost and no clarity.

UI-17.3. Only transitions that explain something may animate: a surface
appearing or disappearing. Decorative motion is prohibited.

UI-17.4. Animation MUST be bounded. `motion_max_frames(dur)` exists so a
transition cannot render forever if the clock stalls or runs backwards. Do not
write an animation loop without a frame bound.

UI-17.5. Animations SHOULD be interruptible. A new input during a transition
must be able to retarget it.

UI-17.6. All motion math is integer, via `motion_progress` / `motion_ease` /
`motion_value`. Do not hand-roll easing with floats.

---

# 18. XML

UI-18.1. XML describes **structure and semantics**. The theme describes
**appearance**. This division is not negotiable.

UI-18.2. Visual attributes are rejected by the inflater and MUST NOT be
attempted:

```xml
<!-- violation: rejected by the inflater -->
<button text="Apply" radius="7" padding="13" color="#2C5EF5" shadow="2"/>
```

UI-18.3. If a visual value cannot be obtained from the theme, the token set is
missing an entry (UI-2.3). It is not a reason to add an XML attribute.

UI-18.4. XML documents are real files under `ui/xml/`, embedded at build time.
They MUST NOT be string literals buried in a `.cpp`:

```cmake
kyuzen_embed_xml(kyuzen-settings SOURCES settings_appearance.xml)
```

```cpp
#include "ui_xml_data.h"   // generated
ui_xml_doc_t* doc = ui_xml_parse(ui_xml_settings_appearance,
                                 ui_xml_settings_appearance_len, &err);
```

A document embedded in code cannot be reviewed, diffed, or syntax-highlighted.

UI-18.5. Attributes available to XML are semantic only:

| Attribute | Values | Meaning |
| --- | --- | --- |
| `variant` | `primary`/`secondary`/`tertiary`/`danger` | action role |
| `icon` | icon name (`settings`, `check`, …) | semantic icon |
| `size` | `sm`/`md`/`lg`/`xl` | icon optical size |
| `spacing` | `xs`/`sm`/`md`/`lg`/`xl` | gap between children |
| `theme-mode`, `theme-accent`, `theme-custom` | | user theme selection |
| `id`, `enabled`, `visible`, `tooltip`, `width`, `height` | | generic |

UI-18.6. `<window>` accepts theme attributes only. Nothing else.

UI-18.7. Element set is fixed: `window`, `vbox`, `hbox`, `grid`, `scrollview`,
`tab`, `section`, `label`, `button`, `textbox`, `checkbox`, `switch`, `radio`,
`combobox` (+`item`), `slider`, `progressbar`, `separator`, `icon`, `image`,
`listview` (+`item`), `menubar` (+`menu`/`item`/`sep`), `toolbar`,
`statusbar`. Unknown elements and unknown attributes are hard errors — do not
add a "silently ignore" path, because a typo that does nothing is worse than a
failure.

UI-18.8. Behavior does not live in XML. Callbacks are bound from C by id:

```c
ui_xml_bind(cx, "apply", UI_XML_ON_CLICK, on_apply, 0);
```

UI-18.9. Menus, toolbars, and other collections are addressed **by name**:

```c
ui_menu_set_checked_id(menu, "view_list", 1);
ui_menu_set_enabled_id(menu, "go_back", 0);
```

Hand-maintained row-index enums that must be kept in sync with an XML document
MUST NOT be reintroduced.

UI-18.10. The inflater is permissive about integers in `spacing` and
`<grid padding>` (any value in range parses). That permissiveness is an
implementation detail, not a contract. XML MUST use tokens (UI-3.4).

UI-18.11. Chrome bars are placed by **kind**, not by document order:
`<menubar>`/`<toolbar>` become top bars and `<statusbar>` becomes a bottom bar,
wherever they appear inside `<window>`.

---

# 19. Rendering Quality

UI-19.1. Text antialiasing MUST be correct and idempotent (see §8).

UI-19.2. Rounded geometry MUST render complete edges, including the case where
the radius equals half the height (circles). The `rrect_border` fast path was
once broken for exactly this case, leaving slider handles, radio rings, and
switch tracks visibly open at both sides.

UI-19.3. Borders are 1px (`border::HAIRLINE`). There is no 2px border and no
per-side border thickness.

UI-19.4. A widget MUST stay inside its own bounds, including focus rings,
selection ink, and text. Damage tracking relies on it.

UI-19.5. Scrollbars use the neutral scrollbar tokens and the standard track
width and minimum thumb length. A custom-drawn scrollbar in an application is a
violation.

UI-19.6. Do not sacrifice rendering correctness for a visual effect. A correct
hard edge beats a subtly wrong soft one.

---

# 20. Performance

UI-20.1. UI changes MUST NOT introduce unnecessary allocations, redraws, layout
passes, shadow recomputation, text re-rasterization, or event-processing
overhead.

UI-20.2. The existing damage/invalidation optimization MUST be preserved. Do
not mark a whole window dirty to refresh a row. Do not call `gui_damage_rect()`
from a text provider — the toolkit owns damage.

UI-20.3. Render cost is budgeted. `tests/host/unit/libui_perf_qa.cpp` enforces:

| Widget | Max cost (screens) |
| --- | --- |
| Button (primary) | 0.010 |
| Button (icon) | 0.010 |
| CheckBox | 0.010 |
| Switch | 0.010 |
| Slider | 0.020 |
| TextBox | 0.020 |
| Section (empty) | 0.010 |
| ListView, 12 rich rows | 0.050 |

UI-20.4. Exceeding a budget is a build failure, not a warning to discuss. If a
change genuinely needs more, raise the budget in the same commit **and explain
why in the commit message** — do not quietly relax it.

UI-20.5. Do not add per-frame work to `draw()`. Expensive work belongs in
`arrange()` or a cached computation, and only when the input actually changed.

---

# 21. Backward Compatibility

UI-21.1. The legacy `ui_theme_t` 6-color path MUST remain pixel-identical. It is
locked by `tests/host/unit/libui_theme_test.cpp`.

UI-21.2. `settings.ui` format versions MUST remain readable:

```text
v0  6 x uint32 0x00RRGGBB, no tag          24 bytes
v1  "KTH1" + 6 x color_t                   28 bytes
v2  "KTH2" + mode + accent + custom RGBA   10 bytes
```

Adding a version is allowed. Breaking the ability to read an older one is not.

UI-21.3. A new visual token MUST NOT change how an existing application renders
unless that application is migrated in the same commit.

UI-21.4. Migrating an application means replacing its UI layer, not rewriting
its business logic. Do not introduce unrelated behavior changes in a UI
migration commit.

---

# 22. Documentation

UI-22.1. A change to the design system MUST update
`docs/design/gui/libui-design-system.md` in the same commit.

UI-22.2. A change that alters the visual language, the token set, or the widget
architecture MUST also update
`docs/design/gui/libui-redesign-report.md`.

UI-22.3. New tokens MUST carry a short comment stating **why** the value is what
it is. A bare `constexpr int X = 13;` with no rationale will be rejected in
review. The existing files are the model: `theme/colors.hpp` explains why
`VARIANT_MIX` moves toward `bg`, and `theme/elevation.hpp` explains why levels
0–2 have zero shadow.

UI-22.4. Comments in toolkit sources are Indonesian and short. Do not write
long explanations inside code — put them in `docs/` and leave a one-line
pointer.

---

# 23. Verification

UI-23.1. A UI change is not complete until all of these pass:

```sh
./build.sh                     # clean build, zero new warnings
./build.sh test                # host tests
./build.sh iso                 # ISO assembles
```

UI-23.2. The relevant host tests MUST pass, and MUST NOT be disabled or
weakened to accommodate a change:

| Test | Locks |
| --- | --- |
| `test-libui-theme` | token derivation, legacy pixel-identity, mode × accent |
| `test-libui-xml` | inflater semantics, attribute rejection (`G07`), chrome placement |
| `test-libui-fileman_widgets` | widget behavior on real app widgets |
| `test-libui-owner` | widget ownership propagation |
| `test-libui-visual-qa` | production widgets render to PPM without corruption |
| `test-libui-perf-qa` | the render budgets in UI-20.3 |
| `test-text` | text rasterization, including draw idempotency (§8) |
| `test-desktop` | desktop manifests and the crash-notice lifecycle |

UI-23.3. Interactive changes MUST be verified in QEMU. Host tests can prove the
math; they cannot prove the UI responds.

UI-23.4. Both themes MUST be checked. A change verified only in dark mode is
not verified — light mode is where recessed/raised surfaces and borders most
often collapse.

UI-23.5. A visual claim MUST be backed by evidence. "Looks modern" is not
validation. Acceptable evidence: a host test, a PPM/visual-QA artifact, a QEMU
probe result, or a measurement. See `RULES.md` §2 and the review checklist.

UI-23.6. When verifying text or geometry changes, compare **pixels** rather than
trusting the layout numbers. A rendered comparison between a fresh draw and a
repainted region is the check that catches the idempotency class of bug (§8).

---

# 24. Review Checklist

Run this on every UI pull request.

```text
[ ] No hardcoded color, radius, spacing, font size, or icon geometry
[ ] Every visual value traces to a token in theme/
[ ] New token (if any) carries a comment explaining why that value
[ ] Spacing values are on the scale, or documented per UI-3.3
[ ] Radius uses one of the defined levels; PILL only where it means something
[ ] Control heights come from control:: tokens; side-by-side controls match
[ ] Text uses TypeRole; overflow uses text_ellipsis()
[ ] Text compositing is idempotent; surface_rect() used where a bg is painted
[ ] No widget draws its children twice per frame
[ ] All applicable states defined: hover, pressed, focused, disabled, selected
[ ] Focus is visible while hovering; disabled wins over hover
[ ] Keyboard reachable; Tab order is visual order; composite keys consumed
[ ] No shadow outside ELEV_POPUP / ELEV_DIALOG; effects not stacked
[ ] No gradient in new UI; no emoji or ad-hoc glyphs as icons
[ ] Rows are not wrapped in per-row cards; settings use section + list
[ ] At most one primary button per surface
[ ] XML is structural; no visual attributes; XML lives in ui/xml/
[ ] Widgets stay inside their bounds; damage tracking preserved
[ ] Render budgets still pass (test-libui-perf-qa)
[ ] Verified in BOTH light and dark
[ ] Verified in QEMU for interactive changes
[ ] Builds clean; host tests pass; no test disabled or weakened
[ ] Docs updated (design system, and report if the language changed)
[ ] No kernel changes introduced by a UI change
```

---

# 25. Known Deviations

These are real, current violations of the rules above. They are recorded so
nobody mistakes them for precedent. Fixing them is welcome; copying them is not.

UI-25.1. `libs/gui/widget/include/editor/textedit.hpp` hardcodes the terminal
prompt color (`ps1_color(COLOR_HEX(0x7CC7FF))`). It predates the token system
and should become a theme token.

UI-25.2. The XML inflater accepts raw integers for `spacing` and `<grid
padding>` (UI-18.10). The rules forbid them; the parser does not yet.

UI-25.3. `system/desktop/theme.hpp` keeps its own constants that *mirror*
`libui` values (marked `== libui surface_hover`, `== libui border`, and so on).
The desktop shell draws through `libgui` rather than through widgets
(UI-13.3), so it cannot read `ui::Theme` directly. When a `libui` token changes,
these mirrors MUST be updated in the same commit — they are a synchronization
hazard, not an independent palette.

UI-25.4. `TypeRole::ft_size_px` and `ft_weight` are declared intent that the
toolkit's own bitmap path does not apply (UI-6.7). Roles currently influence
line height, tracking, case, and tone only.

UI-25.5. Applications other than Settings and File Manager do not yet call
`ui::uifont_install()`, so they still render in the bitmap font regardless of
the user's font setting.
