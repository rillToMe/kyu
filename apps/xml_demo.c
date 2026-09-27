// apps/xml_demo.c — konsumen nyata XML deklaratif (Phase E).
//
// Seluruh UI dideklarasikan sebagai string XML, diinflate ke widget NATIVE
// lewat libui_xml (parse -> validate -> inflate -> libui biasa), lalu
// callback diikat dari C (tanpa kode di XML). Bukti: XML = konsumen libui.
//
// Build: xml_demo.o + userlib + libgui + widget + png (GUI_APPS).
#include "userlib.h"
#include "libui.h"
#include "libui_xml.h"

static ui_window_t* win;
static ui_xml_ctx_t* xctx;
static ui_widget_t* status;

static void set_status(const char* s) {
    if (status) ui_label_set_text(status, s);
}

static void on_apply(void* u) { (void)u; set_status("Applied from XML"); }
static void on_cancel(void* u) { (void)u; ui_window_request_close(win); }
static void on_notif(void* u) { (void)u; set_status("Notifications toggled"); }
static void on_mode(void* u) { (void)u; set_status("Mode changed"); }
static void on_theme(void* u) { (void)u; set_status("Theme picked"); }

// Layar settings-like (§53): tema + grid + radio grup + combo + checkbox +
// textbox + slider + separator + tombol. Satu string, tanpa builder C.
static const char UI_DOC[] =
    "<window theme-mode=\"dark\" theme-accent=\"purple\">"
    "<vbox spacing=\"md\">"
    "<label text=\"XML Settings\"/>"
    "<grid rows=\"3\" cols=\"2\" gap=\"8\">"
    "<label row=\"0\" col=\"0\" text=\"Theme\"/>"
    "<combobox id=\"theme\" row=\"0\" col=\"1\" width=\"140\">"
    "<item text=\"Dark\"/><item text=\"Light\"/>"
    "</combobox>"
    "<label row=\"1\" col=\"0\" text=\"Notify\"/>"
    "<checkbox id=\"notif\" row=\"1\" col=\"1\" text=\"Enabled\"/>"
    "<label row=\"2\" col=\"0\" text=\"Mode\"/>"
    "<hbox row=\"2\" col=\"1\" spacing=\"sm\">"
    "<radio id=\"m1\" text=\"Basic\" group=\"mode\" selected=\"true\"/>"
    "<radio id=\"m2\" text=\"Advanced\" group=\"mode\"/>"
    "</hbox>"
    "</grid>"
    "<separator/>"
    "<textbox id=\"name\" width=\"200\" text=\"kyuzen\" tooltip=\"User name\"/>"
    "<slider id=\"vol\" min=\"0\" max=\"10\" value=\"7\"/>"
    "<hbox spacing=\"sm\">"
    "<button id=\"apply\" text=\"Apply\" variant=\"primary\"/>"
    "<button id=\"cancel\" text=\"Cancel\"/>"
    "</hbox>"
    "<label id=\"status\" text=\"Ready\"/>"
    "</vbox>"
    "</window>";

int main(void) {
    win = ui_window_create(460, 380);
    if (!win) sys_exit_code(1);
    ui_window_set_title(win, "XML Demo");

    unsigned n = 0;
    while (UI_DOC[n]) n++;
    ui_xml_error_t err;
    ui_xml_doc_t* doc = ui_xml_parse(UI_DOC, n, &err);
    if (!doc) sys_exit_code(1);
    xctx = ui_xml_ctx_create(win);
    if (!xctx) { ui_xml_doc_destroy(doc); sys_exit_code(1); }
    if (!ui_xml_inflate(xctx, doc, &err)) {
        ui_xml_ctx_destroy(xctx);
        ui_xml_doc_destroy(doc);
        sys_exit_code(1);
    }
    ui_xml_doc_destroy(doc);   // UI hidup tanpa dokumen (independen)

    status = ui_xml_find(xctx, "status");
    ui_xml_bind(xctx, "apply", UI_XML_ON_CLICK, on_apply, 0);
    ui_xml_bind(xctx, "cancel", UI_XML_ON_CLICK, on_cancel, 0);
    ui_xml_bind(xctx, "notif", UI_XML_ON_CHANGE, on_notif, 0);
    ui_xml_bind(xctx, "m1", UI_XML_ON_CHANGE, on_mode, 0);
    ui_xml_bind(xctx, "m2", UI_XML_ON_CHANGE, on_mode, 0);
    ui_xml_bind(xctx, "theme", UI_XML_ON_CHANGE, on_theme, 0);

    ui_window_run(win);
    ui_window_destroy(win);   // grup radio + widget milik window
    ui_xml_ctx_destroy(xctx); // konteks mati setelah window (urutan dokumen)
    sys_exit();   // kontrak ENTRY(main): tak boleh return (ret = #GP)
}
