/* ============================================================
 * KyuzenOS - Lexbor Stage A QEMU smoke probe (userspace, freestanding)
 *
 * Tujuan: membuktikan bahwa liblexbor_kyuzen.a (Lexbor 3.0.0 HTML/DOM)
 * BENAR-BENAR berjalan di dalam userspace KyuzenOS - bukan hanya di host.
 * Ini adalah closure yang sama dengan build freestanding: core/dom/html/
 * ns/tag tanpa conv/dtoa/strtod/diyfp, allocator lewat Kyuzen libc
 * (malloc/calloc/realloc/free -> sys_alloc/sys_free).
 *
 * Alur (dijalankan via `start lexbor_phase_a`):
 *   1. create parser + document, parse HTML well-formed
 *   2. DOM lookup (by_tag_name / by_id) + baca teks + atribut
 *   3. serialisasi balik ke HTML
 *   4. parse input rusak (tidak boleh crash)
 *   5. parse kosong (tetap menghasilkan <body>)
 *   6. destroy bersih
 *
 * Observability: syscall Kyuzen #49 (write) ke fd 1 -> TTY + mirror COM1.
 * Marker: "[lexbor-a] PASS" / "[lexbor-a] FAIL <tag>".
 *
 * Build: make lexbor-stage-a-qemu
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>

#include "lexbor/html/html.h"
#include "lexbor/html/interfaces/document.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/attr.h"
#include "lexbor/dom/collection.h"

/* ------------------------------------------------------------
 * Syscall Kyuzen (int 0x80): RAX=nomor, RBX/RCX/RDX=arg1..3
 * ------------------------------------------------------------ */
static int kz_write(int fd, const void *buf, uint32_t n) {
    int64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"((uint64_t)49), "b"((uint64_t)fd),
                       "c"((uint64_t)buf), "d"((uint64_t)n));
    return (int)ret;
}

static void say(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') { n++; }
    kz_write(1, s, (uint32_t)n);
}

static void say_num(long v) {
    char buf[24];
    int i = (int)sizeof(buf);
    unsigned long u;
    int neg = 0;
    if (v < 0) { neg = 1; u = (unsigned long)(-v); } else { u = (unsigned long)v; }
    buf[--i] = '\0';
    if (u == 0) { buf[--i] = '0'; }
    while (u > 0) { buf[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) { buf[--i] = '-'; }
    say(&buf[i]);
}

#define FAIL(tag) do { say("[lexbor-a] FAIL "); say(tag); say("\n"); return 42; } while (0)

/* ---- serializer sink: akumulasi ke buffer statis ---- */
static char  g_ser[4096];
static size_t g_ser_len = 0;

static lxb_status_t serialize_cb(const lxb_char_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (g_ser_len + len < sizeof(g_ser) - 1) {
        for (size_t i = 0; i < len; i++) {
            g_ser[g_ser_len++] = (char)data[i];
        }
        g_ser[g_ser_len] = '\0';
    }
    return LXB_STATUS_OK;
}

static int mem_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) { return 0; } }
    return 1;
}

/* ============================================================
 * 1-3. well-formed: parse -> DOM lookup -> serialize
 * ============================================================ */
static int test_wellformed(void) {
    static const char html[] =
        "<!DOCTYPE html><html><head><title>Kyuzen</title></head>"
        "<body><h1 id=\"t\">Hello</h1>"
        "<p class=\"c\">World</p>"
        "<a href=\"/x\">link</a></body></html>";

    say("[lexbor-a] start\n");

    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (parser == NULL) { FAIL("parser_create"); }
    if (lxb_html_parser_init(parser) != LXB_STATUS_OK) { FAIL("parser_init"); }

    lxb_html_document_t *doc =
        lxb_html_parse(parser, (const lxb_char_t *)html, sizeof(html) - 1);
    if (doc == NULL) { FAIL("parse=null"); }
    say("[lexbor-a] ok parse\n");

    lxb_dom_node_t *root = lxb_dom_interface_node(doc);
    if (root == NULL) { FAIL("root=null"); }

    /* <title> text */
    lxb_dom_collection_t *col = lxb_dom_collection_create(&doc->dom_document);
    if (col == NULL) { FAIL("collection_create"); }

    if (lxb_dom_node_by_tag_name(root, col,
                                 (const lxb_char_t *)"title", 5) != LXB_STATUS_OK) {
        FAIL("by_tag_name");
    }
    if (lxb_dom_collection_length(col) != 1) { FAIL("title-count"); }
    {
        lxb_dom_node_t *title = lxb_dom_collection_node(col, 0);
        size_t tlen = 0;
        const lxb_char_t *text = lxb_dom_node_text_content(title, &tlen);
        if (text == NULL || tlen != 6 || !mem_eq((const char *)text, "Kyuzen", 6)) {
            FAIL("title-text");
        }
    }
    say("[lexbor-a] ok dom-lookup (title text)\n");

    /* <h1 id="t"> via by_id */
    {
        lxb_dom_node_t *h1node =
            lxb_dom_node_by_id(root, (const lxb_char_t *)"t", 1);
        if (h1node == NULL) { FAIL("by_id"); }
        lxb_dom_element_t *h1 = lxb_dom_interface_element(h1node);
        size_t alen = 0;
        const lxb_char_t *idv = lxb_dom_element_get_attribute(
            h1, (const lxb_char_t *)"id", 2, &alen);
        if (idv == NULL || alen != 1 || idv[0] != 't') { FAIL("h1-attr"); }

        size_t tlen = 0;
        const lxb_char_t *text = lxb_dom_node_text_content(h1node, &tlen);
        if (text == NULL || tlen != 5 || !mem_eq((const char *)text, "Hello", 5)) {
            FAIL("h1-text");
        }
    }
    say("[lexbor-a] ok dom-lookup (by_id + attr)\n");

    /* <a href="/x"> */
    lxb_dom_collection_clean(col);
    if (lxb_dom_node_by_tag_name(root, col, (const lxb_char_t *)"a", 1)
            != LXB_STATUS_OK || lxb_dom_collection_length(col) != 1) {
        FAIL("anchor-count");
    }
    {
        lxb_dom_element_t *a = lxb_dom_collection_element(col, 0);
        size_t vlen = 0;
        const lxb_char_t *href = lxb_dom_element_get_attribute(
            a, (const lxb_char_t *)"href", 4, &vlen);
        if (href == NULL || vlen != 2 || !mem_eq((const char *)href, "/x", 2)) {
            FAIL("anchor-href");
        }
    }

    /* serialize */
    g_ser_len = 0;
    g_ser[0] = '\0';
    if (lxb_html_serialize_deep_cb(root, serialize_cb, NULL) != LXB_STATUS_OK) {
        FAIL("serialize");
    }
    if (g_ser_len == 0) { FAIL("serialize-empty"); }
    say("[lexbor-a] ok serialize ("); say_num((long)g_ser_len); say(" bytes)\n");

    lxb_dom_collection_destroy(col, true);
    lxb_html_document_destroy(doc);
    lxb_html_parser_destroy(parser);
    return 0;
}

/* ============================================================
 * 4. malformed input - must not crash
 * ============================================================ */
static int test_malformed(void) {
    static const char html[] =
        "<html><body><div><p>a<span>b</div></p><table><tr><td>x"
        "<b>bold</i>text";

    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (parser == NULL) { FAIL("malformed:parser_create"); }
    lxb_html_parser_init(parser);

    lxb_html_document_t *doc =
        lxb_html_parse(parser, (const lxb_char_t *)html, sizeof(html) - 1);
    if (doc == NULL) { FAIL("malformed:parse=null"); }

    lxb_dom_node_t *root = lxb_dom_interface_node(doc);
    g_ser_len = 0;
    g_ser[0] = '\0';
    if (lxb_html_serialize_deep_cb(root, serialize_cb, NULL) != LXB_STATUS_OK) {
        FAIL("malformed:serialize");
    }
    if (g_ser_len == 0) { FAIL("malformed:serialize-empty"); }

    lxb_html_document_destroy(doc);
    lxb_html_parser_destroy(parser);
    say("[lexbor-a] ok malformed\n");
    return 0;
}

/* ============================================================
 * 5. empty input
 * ============================================================ */
static int test_empty(void) {
    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (parser == NULL) { FAIL("empty:parser_create"); }
    lxb_html_parser_init(parser);

    lxb_html_document_t *doc =
        lxb_html_parse(parser, (const lxb_char_t *)"", 0);
    if (doc == NULL) { FAIL("empty:parse=null"); }
    if (lxb_html_document_body_element(doc) == NULL) { FAIL("empty:no-body"); }

    lxb_html_document_destroy(doc);
    lxb_html_parser_destroy(parser);
    say("[lexbor-a] ok empty\n");
    return 0;
}

int main(void) {
    int r;
    r = test_wellformed(); if (r != 0) { return r; }
    r = test_malformed();  if (r != 0) { return r; }
    r = test_empty();      if (r != 0) { return r; }

    say("[lexbor-a] PASS\n");
    return 0;
}
