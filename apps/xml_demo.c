// apps/xml_demo.c - konsumen nyata XML deklaratif (Phase E).
//
// UI dideklarasikan sebagai DOKUMEN XML SUNGGUHAN di
// `ui/xml/xml_demo.xml` (bukan string literal di file ini), lalu di-embed saat
// build oleh cmake/KyuzenUiXml.cmake. Alasannya sederhana: XML yang tersembunyi
// di dalam .c/.cpp tidak bisa di-review sebagai dokumen, tidak enak di-diff,
// dan editor tidak memberinya penyorotan sintaks.
//
// Alurnya tetap: parse -> validate -> inflate -> libui biasa, dengan callback
// diikat dari C (tanpa kode di XML). Bukti: XML = konsumen libui.
//
// Build: xml_demo.o + userlib + libgui + widget + png (GUI_APPS).
#include "userlib.h"
#include "libui.h"
#include "libui_xml.h"

#include "ui_xml_data.h"   // generated dari ui/xml/xml_demo.xml

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

// Kontrak ENTRY(main): fungsi ini TIDAK BOLEH kembali (ret = #GP). Karena
// `sys_exit()` tidak ditandai noreturn oleh userlib.h, dipakai bentuk
// `void main(void)` seperti widget_demo/notepad/terminal — dengan begitu
// compiler tidak perlu membuktikan bahwa semua jalur keluar, dan tidak ada
// peringatan -Wreturn-type yang menyembunyikan masalah nyata di masa depan.
void main(void) {
    win = ui_window_create(460, 400);
    if (!win) sys_exit_code(1);
    ui_window_set_title(win, "XML Demo");

    ui_xml_error_t err;
    ui_xml_doc_t* doc = ui_xml_parse(ui_xml_xml_demo, ui_xml_xml_demo_len, &err);
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
