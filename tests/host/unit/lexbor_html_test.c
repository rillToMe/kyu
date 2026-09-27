/*
 * tests/host/unit/lexbor_html_test.c
 *
 * Stage A host smoke test for the vendored Lexbor 3.0.0 HTML/DOM build.
 *
 * PURPOSE
 * -------
 * Prove that the *exact* source set compiled into liblexbor_kyuzen.a can
 *  1. create a parser + document,
 *  2. parse well-formed HTML into a DOM tree,
 *  3. look elements up (by tag name / by id) and read text + attributes,
 *  4. serialize back to HTML,
 *  5. survive malformed input without crashing,
 *  6. tear everything down cleanly.
 *
 * It is compiled with HOSTCC against the SAME source files used for the
 * freestanding archive (LEXBOR_HOST_SRCS), with the same exclusion of
 * the FP files (conv/dtoa/strtod/diyfp) and the same int64 conv shim.
 * So the code paths exercised here are byte-for-byte the ones that run
 * in the Kyuzen userspace build; only the ABI (host vs none-elf) and the
 * allocator backing differ.
 *
 * Exit code 0 = all checks pass. Non-zero = first failing check printed.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lexbor/html/html.h"
#include "lexbor/html/interfaces/document.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/attr.h"
#include "lexbor/dom/collection.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL: %s\n", (msg));                           \
            g_fail++;                                                         \
        } else {                                                              \
            fprintf(stdout, "  ok  : %s\n", (msg));                           \
        }                                                                     \
    } while (0)

/* serialize callback: append into a fixed buffer */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} sbuf_t;

static lxb_status_t
serialize_cb(const lxb_char_t *data, size_t len, void *ctx)
{
    sbuf_t *sb = (sbuf_t *) ctx;

    if (sb->len + len < sb->cap) {
        memcpy(sb->buf + sb->len, data, len);
        sb->len += len;
        sb->buf[sb->len] = '\0';
    }

    return LXB_STATUS_OK;
}

static void
test_wellformed(void)
{
    static const char html[] =
        "<!DOCTYPE html><html><head><title>Kyuzen</title></head>"
        "<body><h1 id=\"t\">Hello</h1>"
        "<p class=\"c\">World</p>"
        "<a href=\"/x\">link</a></body></html>";

    fprintf(stdout, "[1] well-formed parse\n");

    lxb_html_parser_t *parser = lxb_html_parser_create();
    CHECK(parser != NULL, "parser_create");
    if (parser == NULL) { return; }

    CHECK(lxb_html_parser_init(parser) == LXB_STATUS_OK, "parser_init");

    lxb_html_document_t *doc =
        lxb_html_parse(parser, (const lxb_char_t *) html, sizeof(html) - 1);
    CHECK(doc != NULL, "lxb_html_parse returns document");
    if (doc == NULL) { lxb_html_parser_destroy(parser); return; }

    /* title */
    lxb_dom_collection_t *col = lxb_dom_collection_create(&doc->dom_document);
    CHECK(col != NULL, "collection_create");

    lxb_dom_node_t *root = lxb_dom_interface_node(doc);
    CHECK(root != NULL, "document root node");

    lxb_status_t st = lxb_dom_node_by_tag_name(root, col,
                                               (const lxb_char_t *) "title", 5);
    CHECK(st == LXB_STATUS_OK, "by_tag_name(title)");
    CHECK(lxb_dom_collection_length(col) == 1, "exactly one <title>");

    if (lxb_dom_collection_length(col) == 1) {
        lxb_dom_node_t *title = lxb_dom_collection_node(col, 0);
        size_t tlen = 0;
        const lxb_char_t *text = lxb_dom_node_text_content(title, &tlen);
        CHECK(text != NULL && tlen == 6 && memcmp(text, "Kyuzen", 6) == 0,
              "title text == \"Kyuzen\"");
    }

    /* h1 by id (returns the node directly, not a collection) */
    lxb_dom_node_t *h1node =
        lxb_dom_node_by_id(root, (const lxb_char_t *) "t", 1);
    CHECK(h1node != NULL, "by_id(t)");

    if (h1node != NULL) {
        lxb_dom_element_t *h1 = lxb_dom_interface_element(h1node);
        size_t alen = 0;
        const lxb_char_t *idv = lxb_dom_element_get_attribute(h1,
                                     (const lxb_char_t *) "id", 2, &alen);
        CHECK(idv != NULL && alen == 1 && idv[0] == 't',
              "h1 attribute id == \"t\"");

        size_t tlen = 0;
        const lxb_char_t *text = lxb_dom_node_text_content(h1node, &tlen);
        CHECK(text != NULL && tlen == 5 && memcmp(text, "Hello", 5) == 0,
              "h1 text == \"Hello\"");
    }

    /* anchor attribute */
    lxb_dom_collection_clean(col);
    st = lxb_dom_node_by_tag_name(root, col, (const lxb_char_t *) "a", 1);
    CHECK(st == LXB_STATUS_OK && lxb_dom_collection_length(col) == 1,
          "exactly one <a>");
    if (lxb_dom_collection_length(col) == 1) {
        lxb_dom_element_t *a = lxb_dom_collection_element(col, 0);
        size_t vlen = 0;
        const lxb_char_t *href = lxb_dom_element_get_attribute(a,
                                     (const lxb_char_t *) "href", 4, &vlen);
        CHECK(href != NULL && vlen == 2 && memcmp(href, "/x", 2) == 0,
              "a href == \"/x\"");
    }

    /* serialize round-trip */
    char out[1024];
    sbuf_t sb = { out, sizeof(out), 0 };
    out[0] = '\0';
    st = lxb_html_serialize_deep_cb(root, serialize_cb, &sb);
    CHECK(st == LXB_STATUS_OK, "serialize_deep_cb");
    CHECK(sb.len > 0, "serialized output non-empty");
    CHECK(strstr(out, "Kyuzen") != NULL, "serialized contains title text");
    CHECK(strstr(out, "href=\"/x\"") != NULL, "serialized contains href");
    fprintf(stdout, "  serialized: %s\n", out);

    lxb_dom_collection_destroy(col, true);
    lxb_html_document_destroy(doc);
    lxb_html_parser_destroy(parser);
}

static void
test_malformed(void)
{
    /* deliberately broken: unclosed tags, stray '<', missing end */
    static const char html[] =
        "<html><body><div><p>a<span>b</div></p><table><tr><td>x"
        "<b>bold</i>text";

    fprintf(stdout, "[2] malformed parse (must not crash)\n");

    lxb_html_parser_t *parser = lxb_html_parser_create();
    CHECK(parser != NULL, "parser_create");
    if (parser == NULL) { return; }
    CHECK(lxb_html_parser_init(parser) == LXB_STATUS_OK, "parser_init");

    lxb_html_document_t *doc =
        lxb_html_parse(parser, (const lxb_char_t *) html, sizeof(html) - 1);
    CHECK(doc != NULL, "parse returns document for malformed input");

    if (doc != NULL) {
        lxb_dom_node_t *root = lxb_dom_interface_node(doc);
        char out[2048];
        sbuf_t sb = { out, sizeof(out), 0 };
        out[0] = '\0';
        lxb_status_t st = lxb_html_serialize_deep_cb(root, serialize_cb, &sb);
        CHECK(st == LXB_STATUS_OK, "serialize malformed DOM");
        CHECK(sb.len > 0, "malformed DOM serializes non-empty");
        lxb_html_document_destroy(doc);
    }

    lxb_html_parser_destroy(parser);
}

static void
test_empty(void)
{
    fprintf(stdout, "[3] empty input\n");

    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (parser == NULL) { g_fail++; return; }
    lxb_html_parser_init(parser);

    lxb_html_document_t *doc =
        lxb_html_parse(parser, (const lxb_char_t *) "", 0);
    CHECK(doc != NULL, "parse empty returns document");
    if (doc != NULL) {
        CHECK(lxb_html_document_body_element(doc) != NULL,
              "empty parse still yields <body>");
        lxb_html_document_destroy(doc);
    }

    lxb_html_parser_destroy(parser);
}

int
main(void)
{
    fprintf(stdout, "lexbor_html_test (Lexbor 3.0.0, HTML/DOM)\n");

    test_wellformed();
    test_malformed();
    test_empty();

    if (g_fail == 0) {
        fprintf(stdout, "\nALL PASS\n");
        return 0;
    }

    fprintf(stderr, "\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
