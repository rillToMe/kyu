# XML UI

`libui_xml` (`libs/gui/widget/{include,src}/xml/`, declared in
`include/libui_xml.h`) builds native `libui` widgets from an XML document. It
is a **declarative front end to the existing toolkit**, not a second UI
framework: there is no layout engine, no CSS, no scripting, and no
XPath/DTD/entity support.

## Pipeline

```text
XML text
    │  ui_xml_parse()
    ▼
XML tree (ui_xml_doc_t)
    │  validation + ui_xml_inflate()
    ▼
native libui widgets (created via the normal ui_* API)
```

## Parser

`ui_xml_parse(data, size, err)` parses a freestanding XML subset. The input need
not be NUL-terminated. Errors carry a 1-based line/column and the related
element/attribute name.

### Limits

| Constant | Value | Rationale |
| --- | --- | --- |
| `UI_XML_MAX_DOC` | 65536 | 64 KB document |
| `UI_XML_MAX_DEPTH` | 16 | Nesting depth |
| `UI_XML_MAX_NODES` | 512 | Total nodes |
| `UI_XML_MAX_ATTRS` | 16 | Attributes per element |
| `UI_XML_MAX_CHILDREN` | 64 | Children per element (parser; native limits are lower) |
| `UI_XML_MAX_ATTR_NAME` | 32 | Attribute name length |
| `UI_XML_MAX_ATTR_VALUE` | 256 | Attribute value length |
| `UI_XML_MAX_ID` | 48 | ID length |
| `UI_XML_MAX_IDS` | 32 | IDs per context |
| `UI_XML_MAX_GROUPS` | 8 | Named radio groups per context |
| `UI_XML_MAX_ROOTS` | 16 | Top-level children per window |

### Error codes

`UI_XML_OK` (0) is success. Other codes include `UI_XML_OOM`, `UI_XML_TOO_BIG`,
`UI_XML_TOO_DEEP`, `UI_XML_TOO_MANY_NODES`, `UI_XML_TOO_MANY_ATTRS`,
`UI_XML_TOO_MANY_CHILDREN`, `UI_XML_ATTR_TOO_LONG`, `UI_XML_SYNTAX`,
`UI_XML_MISMATCH`, `UI_XML_UNCLOSED`, `UI_XML_BAD_ENTITY`, `UI_XML_UNSUPPORTED`
(CDATA/DOCTYPE/PI outside the subset), `UI_XML_EMPTY`, `UI_XML_MULTI_ROOT`,
`UI_XML_BAD_TEXT`, `UI_XML_UNKNOWN_ELEMENT`, `UI_XML_UNKNOWN_ATTRIBUTE`,
`UI_XML_MISSING_ATTRIBUTE`, `UI_XML_INVALID_VALUE`, `UI_XML_BAD_CHILD`,
`UI_XML_BAD_PLACEMENT`, `UI_XML_DUP_ID`, and `UI_XML_LIMIT`.

## Inflation

Inflation is **transactional**: if any element fails, every widget created
during that call is destroyed and the window is left untouched.

```c
ui_xml_ctx_t *ctx = ui_xml_ctx_create(window);
if (!ui_xml_inflate(ctx, doc, &err)) {
    /* err has the reason; window unchanged */
}
```

- A context owns the ID map and named radio groups for one inflation result.
  Destroy the context **after** the window is destroyed.
- `ui_xml_inflate_detached` builds widgets without parenting them to the
  window, for consumers that reparent (for example, a page inserted into
  another layout). The caller takes ownership via `ui_xml_root_count` /
  `ui_xml_root_at` and must call `ui_xml_release`.
- The document may be destroyed after inflation; the UI does not depend on it.

## Binding

Callbacks are bound from C code — **there is no code in the XML**:

```c
ui_widget_t *w = ui_xml_find(ctx, "submit");
ui_xml_bind(ctx, "submit", UI_XML_ON_CLICK, on_submit, userdata);
```

| Event | Value |
| --- | --- |
| `UI_XML_ON_CLICK` | 0 |
| `UI_XML_ON_CHANGE` | 1 |

The event-to-widget-type mapping is documented per widget; an unsupported
pairing returns 0.

## Where XML lives

UI documents are **real `.xml` files** in `ui/xml/`, not string literals inside
`.c`/`.cpp`. The build embeds them into a header:

```cmake
kyuzen_embed_xml(kyuzen-settings SOURCES settings_appearance.xml)
```

```cpp
#include "ui_xml_data.h"   // generated
ui_xml_doc_t* doc = ui_xml_parse(ui_xml_settings_appearance,
                                 ui_xml_settings_appearance_len, &err);
```

Why: XML buried in source cannot be reviewed as a document, diffs poorly, and
gets no syntax highlighting. Tests embed the same file, so the document and the
test can never drift apart.

See `cmake/KyuzenUiXml.cmake` and `cmake/embed_xml.cmake`.

## What XML owns, and what it does not

XML owns **structure and semantics**. The theme owns **appearance**.

| Semantic attribute | Values | Meaning |
| --- | --- | --- |
| `variant` | `primary`, `secondary`, `tertiary`, `danger` | action role |
| `icon` | icon name (`settings`, `check`, `chevron-down`, …) | semantic icon |
| `spacing` | `xs`/`sm`/`md`/`lg`/`xl` or px | gap between children |
| `size` | `sm`/`md`/`lg`/`xl` | icon optical size |
| `theme-mode`, `theme-accent` | | user theme choice |

Visual attributes (`padding`, `radius`, `color`, `shadow`, …) are **rejected**
by the inflater with `UI_XML_UNKNOWN_ATTRIBUTE`. If a visual value cannot come
from the theme, the theme needs a new token — not the schema a new attribute.

## Example

```xml
<window>
  <vbox spacing="lg">
    <label text="Appearance"/>

    <section title="Mode" spacing="sm">
      <radio id="mode_dark" text="Dark" group="mode" selected="true"/>
      <radio id="mode_light" text="Light" group="mode"/>
    </section>

    <section title="Accent color" spacing="sm">
      <hbox spacing="sm">
        <button id="accent_neutral" text="Neutral"/>
        <button id="accent_blue" text="Blue" variant="primary"/>
      </hbox>
      <label id="status" text="Ready"/>
    </section>
  </vbox>
</window>
```

## Elements

`window`, `vbox`, `hbox`, `grid`, `section`, `label`, `button`, `textbox`,
`checkbox`, `switch`, `radio`, `combobox` (+ `item`), `slider`, `progressbar`,
`separator`, `icon`, `image`, `listview` (+ `item`).

The set is a fixed schema; unknown elements or attributes are rejected with
`UI_XML_UNKNOWN_ELEMENT` / `UI_XML_UNKNOWN_ATTRIBUTE`.

## Related Documentation

- [GUI Overview](README.md)
- [Design System](../design/gui/libui-design-system.md) — XML rules in context
- [Widget Toolkit](widget-toolkit.md)
- [libdesktop](libdesktop.md)
