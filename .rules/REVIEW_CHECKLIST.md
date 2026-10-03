# KyuzenOS Completion Checklist

Part of the KyuzenOS Development Rules. See `RULES.md` for general principles.

---

# Completion Checklist

A task is NOT complete until all items below are true.

[ ] Code compiles successfully

[ ] Documentation updated

[ ] No duplicate logic

[ ] No dead code

[ ] Clean code principles respected

[ ] Existing architecture preserved

[ ] Logging added where appropriate

[ ] Memory safety verified

[ ] Thread safety verified

[ ] Code formatted consistently

[ ] Build completed successfully

Only after every item is complete may the task be considered finished.

---

# UI Checklist

Required for any change under `libs/gui/widget/`, `include/libui*.h`, `apps/`,
`system/desktop/`, or `ui/xml/`.

See `UI.md` for the binding rules and `UI.md` §24 for the full list.

[ ] No hardcoded color, radius, spacing, font size, or icon geometry

[ ] Every visual value traces to a token in `theme/`

[ ] Text uses a `TypeRole`; overflow uses `text_ellipsis()`

[ ] Text compositing is idempotent (see `UI.md` §8)

[ ] All applicable states defined: hover, pressed, focused, disabled, selected

[ ] Focus visible while hovering; disabled wins over hover

[ ] Keyboard reachable, Tab order is visual order

[ ] No shadow outside `ELEV_POPUP` / `ELEV_DIALOG`; effects not stacked

[ ] No gradient in new UI; no emoji or ad-hoc glyphs as icons

[ ] Rows not wrapped in per-row cards; settings use section + list

[ ] At most one primary button per surface

[ ] XML structural only; no visual attributes; XML lives in `ui/xml/`

[ ] Render budgets pass (`ctest -R test-libui-perf-qa`)

[ ] Verified in BOTH light and dark

[ ] Verified in QEMU for interactive changes

[ ] Host tests pass; no test disabled or weakened

[ ] Design system docs updated (`docs/design/gui/libui-design-system.md`)

[ ] No kernel changes introduced by a UI change
