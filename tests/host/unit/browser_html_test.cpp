// browser_html_test — host tests for HTML parser + DOM.
// Run: make test-browser-html
#include <cstdio>
#include <string>

#include "../../../apps/browser/engine/html.cpp"

using namespace browser;

static int failures;
static const char* cur_test;
#define CHECK(c) do { \
    if (!(c)) { printf("FAIL [%s] line %d: %s\n", cur_test, __LINE__, #c); failures++; } \
} while (0)
#define T(name) static void name(void)

static dom::Document doc_of(const std::string& s) {
    dom::Document d;
    html::parse(s, d);
    return d;
}

static std::string all_text(const dom::Node* n) {
    std::string s;
    dom::inner_text(n, s);
    return s;
}

T(t_basic) {
    cur_test = "basic";
    dom::Document d = doc_of("<html><head><title>Hi</title></head><body><p>Hello</p></body></html>");
    CHECK(!d.truncated && d.title == "Hi");
    std::vector<const dom::Node*> ps;
    dom::find_all(d.root.get(), "p", ps);
    CHECK(ps.size() == 1 && all_text(ps[0]) == "Hello");
}

T(t_forgiving_p) {
    cur_test = "forgiving_p";
    dom::Document d = doc_of("<p>Hello<p>World");
    std::vector<const dom::Node*> ps;
    dom::find_all(d.root.get(), "p", ps);
    CHECK(ps.size() == 2);
    CHECK(!d.truncated);
}

T(t_stray_close) {
    cur_test = "stray_close";
    dom::Document d = doc_of("a</div>b</p>c");
    CHECK(all_text(d.root.get()) == "abc");
}

T(t_unclosed) {
    cur_test = "unclosed";
    dom::Document d = doc_of("<div><span>text");
    CHECK(all_text(d.root.get()) == "text");
}

T(t_void) {
    cur_test = "void";
    dom::Document d = doc_of("a<br>b<hr>c<img src=\"x.png\">d");
    CHECK(all_text(d.root.get()) == "abcd");
    std::vector<const dom::Node*> imgs;
    dom::find_all(d.root.get(), "img", imgs);
    CHECK(imgs.size() == 1);
    const std::string* src = imgs[0]->get_attr("src");
    CHECK(src && *src == "x.png");
    CHECK(imgs[0]->children.empty());
}

T(t_attrs) {
    cur_test = "attrs";
    dom::Document d =
        doc_of("<a HREF=\"/x?a=1&amp;b=2\" title='hi' novalue data-a=1 data-a=2>A</a>");
    std::vector<const dom::Node*> as;
    dom::find_all(d.root.get(), "a", as);
    CHECK(as.size() == 1);
    const std::string* href = as[0]->get_attr("href");
    CHECK(href && *href == "/x?a=1&b=2");  // entities in attrs decoded
    const std::string* novalue = as[0]->get_attr("novalues");
    CHECK(novalue == nullptr);
    const std::string* nv = as[0]->get_attr("novalue");
    CHECK(nv && nv->empty());
    const std::string* da = as[0]->get_attr("data-a");
    CHECK(da && *da == "1");  // first duplicate wins
}

T(t_entities) {
    cur_test = "entities";
    dom::Document d = doc_of("<p>&lt;&gt;&amp;&quot;&apos;&nbsp;&#65;&#x42;&foo;</p>");
    std::vector<const dom::Node*> ps;
    dom::find_all(d.root.get(), "p", ps);
    CHECK(ps.size() == 1);
    CHECK(all_text(ps[0]) == "<>&\"' AB&foo;");  // nbsp->space, unknown kept
}

T(t_comments_doctype) {
    cur_test = "comments_doctype";
    dom::Document d = doc_of("<!DOCTYPE html><!-- hi --><p>x</p><!-- unterminated");
    std::vector<const dom::Node*> ps;
    dom::find_all(d.root.get(), "p", ps);
    CHECK(ps.size() == 1 && all_text(ps[0]) == "x");
}

T(t_script_style) {
    cur_test = "script_style";
    dom::Document d =
        doc_of("<script>if(a<b){x=1;}</script><style>p < b { color: red; }</style><p>ok</p>");
    CHECK(all_text(d.root.get()) == "ok");  // script dropped, style not text
    CHECK(d.styles.size() == 1);
    CHECK(d.styles[0].find("color: red") != std::string::npos);
}

T(t_style_links) {
    cur_test = "style_links";
    dom::Document d = doc_of(
        "<link rel=\"stylesheet\" href=\"/a.css\"><link REL=\"STYLESHEET\" href=\"b.css\">"
        "<link rel=\"icon\" href=\"i.png\"><link href=\"no-rel.css\">");
    CHECK(d.style_hrefs.size() == 2);
    CHECK(d.style_hrefs[0] == "/a.css" && d.style_hrefs[1] == "b.css");
}

T(t_lists_headings) {
    cur_test = "lists_headings";
    dom::Document d = doc_of("<h1>T</h1><ul><li>a<li>b</ul><blockquote>q</blockquote>");
    std::vector<const dom::Node*> lis;
    dom::find_all(d.root.get(), "li", lis);
    CHECK(lis.size() == 2);  // li auto-close
    CHECK(all_text(d.root.get()) == "Tabq");
}

T(t_case) {
    cur_test = "case";
    dom::Document d = doc_of("<DIV><P>X</P></DIV>");
    std::vector<const dom::Node*> divs;
    dom::find_all(d.root.get(), "div", divs);
    CHECK(divs.size() == 1);
}

T(t_bare_lt) {
    cur_test = "bare_lt";
    dom::Document d = doc_of("<p>1 < 2 and a < b</p>");
    CHECK(all_text(d.root.get()) == "1 < 2 and a < b");
}

T(t_unknown_tags) {
    cur_test = "unknown_tags";
    dom::Document d = doc_of("<foo>bar<blink>baz</blink></foo>");
    CHECK(all_text(d.root.get()) == "barbaz");
    std::vector<const dom::Node*> foos;
    dom::find_all(d.root.get(), "foo", foos);
    CHECK(foos.size() == 1);
}

T(t_nesting_cap) {
    cur_test = "nesting_cap";
    std::string s;
    for (int i = 0; i < 200; i++) s += "<div>";
    s += "deep";
    dom::Document d = doc_of(s);
    CHECK(d.truncated);  // depth cap hit, no crash
    CHECK(all_text(d.root.get()) == "deep");
}

T(t_title_first) {
    cur_test = "title_first";
    dom::Document d = doc_of("<title> A </title><title>B</title>");
    CHECK(d.title == "A");
}

// ---------- Stage B: Lexbor adapter tests ----------
// These exercise the Lexbor -> KyuBrowser DOM conversion boundary directly.
// Production html::parse() is now Lexbor-backed; these lock the adapter's
// document mapping, attribute copying, tree ordering, entities, malformed
// recovery, empty input and bounded-size behavior.

T(t_b_lexbor_basic_document) {
    cur_test = "b_lexbor_basic_document";
    dom::Document d = doc_of(
        "<html>\n  <head><title>Kyuzen</title></head>\n"
        "  <body><p>Hello</p></body>\n</html>");
    CHECK(!d.truncated);
    CHECK(d.title == "Kyuzen");
    // Root is always the Document node; <html>/<head>/<body> synthesized.
    CHECK(d.root && d.root->type == dom::NodeType::Document);
    CHECK(dom::find_first(d.root.get(), "html") != nullptr);
    CHECK(dom::find_first(d.root.get(), "head") != nullptr);
    const dom::Node* body = dom::find_first(d.root.get(), "body");
    CHECK(body != nullptr);
    const dom::Node* p = dom::find_first(d.root.get(), "p");
    CHECK(p && p->parent == body);  // real parent/child relationship
    CHECK(all_text(p) == "Hello");
}

T(t_b_lexbor_attributes) {
    cur_test = "b_lexbor_attributes";
    dom::Document d = doc_of("<div id=\"main\" class=\"container\" data-test=\"hello\">Text</div>");
    std::vector<const dom::Node*> divs;
    dom::find_all(d.root.get(), "div", divs);
    CHECK(divs.size() == 1);
    const dom::Node* div = divs[0];
    CHECK(div->tag == "div");
    const std::string* id = div->get_attr("id");
    const std::string* cls = div->get_attr("class");
    const std::string* dt = div->get_attr("data-test");
    CHECK(id && *id == "main");
    CHECK(cls && *cls == "container");
    CHECK(dt && *dt == "hello");
    CHECK(all_text(div) == "Text");
}

T(t_b_lexbor_nesting) {
    cur_test = "b_lexbor_nesting";
    dom::Document d = doc_of("<div><p>Hello <b>world</b></p></div>");
    std::vector<const dom::Node*> divs;
    dom::find_all(d.root.get(), "div", divs);
    CHECK(divs.size() == 1);
    const dom::Node* div = divs[0];
    CHECK(div->children.size() == 1);
    const dom::Node* p = div->children[0].get();
    CHECK(p->type == dom::NodeType::Element && p->tag == "p");
    CHECK(p->parent == div);
    // Exact child ordering: text "Hello " then <b>world</b>.
    CHECK(p->children.size() == 2);
    CHECK(p->children[0]->type == dom::NodeType::Text && p->children[0]->text == "Hello ");
    CHECK(p->children[1]->type == dom::NodeType::Element && p->children[1]->tag == "b");
    CHECK(p->children[1]->parent == p);
    CHECK(all_text(p->children[1].get()) == "world");
    CHECK(all_text(d.root.get()) == "Hello world");
}

T(t_b_lexbor_entities) {
    cur_test = "b_lexbor_entities";
    dom::Document d = doc_of("<p>&lt;&gt;&amp;&quot;&apos;&#65;&#x42;</p>");
    std::vector<const dom::Node*> ps;
    dom::find_all(d.root.get(), "p", ps);
    CHECK(ps.size() == 1);
    // Named + numeric entities decoded to the browser's expected UTF-8 bytes.
    CHECK(all_text(ps[0]) == "<>&\"'AB");
}

T(t_b_lexbor_malformed) {
    cur_test = "b_lexbor_malformed";
    // <p> is auto-closed by <div> (Lexbor recovery); no crash, text preserved.
    dom::Document d = doc_of("<p>Hello<div>World");
    CHECK(all_text(d.root.get()) == "HelloWorld");
    std::vector<const dom::Node*> ps;
    dom::find_all(d.root.get(), "p", ps);
    CHECK(ps.size() == 1 && all_text(ps[0]) == "Hello");
    std::vector<const dom::Node*> divs;
    dom::find_all(d.root.get(), "div", divs);
    CHECK(divs.size() == 1 && all_text(divs[0]) == "World");
}

T(t_b_lexbor_empty) {
    cur_test = "b_lexbor_empty";
    dom::Document d = doc_of("");
    // Existing contract: structural success, valid root, no truncation.
    CHECK(!d.truncated);
    CHECK(d.root && d.root->type == dom::NodeType::Document);
    CHECK(d.title.empty());
    CHECK(d.styles.empty());
    CHECK(d.style_hrefs.empty());
    CHECK(all_text(d.root.get()).empty());
}

T(t_b_lexbor_large_bounded) {
    cur_test = "b_lexbor_large_bounded";
    // Bounded but sizeable: 400 paragraphs with text + attributes. Detects
    // accidental quadratic behavior in the two-tree conversion.
    std::string s;
    for (int i = 0; i < 400; i++) {
        s += "<p id=\"p";
        s += std::to_string(i);
        s += "\">item ";
        s += std::to_string(i);
        s += "</p>";
    }
    dom::Document d = doc_of(s);
    std::vector<const dom::Node*> ps;
    dom::find_all(d.root.get(), "p", ps);
    CHECK(ps.size() == 400);
    const std::string* id = ps[399]->get_attr("id");
    CHECK(id && *id == "p399");
    CHECK(all_text(ps[399]) == "item 399");
}

int main() {
    t_basic();
    t_forgiving_p();
    t_stray_close();
    t_unclosed();
    t_void();
    t_attrs();
    t_entities();
    t_comments_doctype();
    t_script_style();
    t_style_links();
    t_lists_headings();
    t_case();
    t_bare_lt();
    t_unknown_tags();
    t_nesting_cap();
    t_title_first();
    t_b_lexbor_basic_document();
    t_b_lexbor_attributes();
    t_b_lexbor_nesting();
    t_b_lexbor_entities();
    t_b_lexbor_malformed();
    t_b_lexbor_empty();
    t_b_lexbor_large_bounded();
    if (failures == 0)
        printf("browser-html: ALL PASS\n");
    else
        printf("browser-html: %d FAILURES\n", failures);
    return failures != 0;
}
