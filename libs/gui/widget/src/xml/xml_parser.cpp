// libs/widget/src/xml/xml_parser.cpp — parser XML freestanding (Phase E).
//
// Subset deterministik: elemen buka/tutup/self-closing, atribut quote
// tunggal/ganda, nesting, whitespace, entity 5 dasar, deklarasi <?xml?>
// di awal, komentar <!-- -->. BUKAN XML 1.0: tanpa DTD/entities kustom/
// namespace/XPath/CDATA/DOCTYPE/PI lain (semua = error deterministik).
//
// Hanya runtime/memory.hpp (tanpa widget, tanpa libc, tanpa STL).
// Setiap `new` null-checked (-> OOM); depth/count bounded.
#include "xml/xml.hpp"

namespace ui {
namespace xml {

void set_error(Error* e, int code, int line, int col,
               const char* elem, const char* attr) {
    if (!e) return;
    e->code = code; e->line = line; e->column = col;
    e->element[0] = '\0'; e->attribute[0] = '\0';
    if (elem) {
        int i = 0;
        while (elem[i] && i < 47) { e->element[i] = elem[i]; i++; }
        e->element[i] = '\0';
    }
    if (attr) {
        int i = 0;
        while (attr[i] && i < 39) { e->attribute[i] = attr[i]; i++; }
        e->attribute[i] = '\0';
    }
}

// --- cursor ---
struct Cur {
    const char* p;
    const char* end;
    int line, col;
    int nnodes;
    Error* err;
};

static int at_end(Cur* c) { return c->p >= c->end; }
static char peek(Cur* c) { return at_end(c) ? '\0' : *c->p; }
static void bump(Cur* c) {
    if (at_end(c)) return;
    if (*c->p == '\n') { c->line++; c->col = 1; }
    else c->col++;
    c->p++;
}
static void skip_ws(Cur* c) {
    for (;;) {
        char ch = peek(c);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') bump(c);
        else break;
    }
}
static int is_name_start(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}
static int is_name_char(char ch) {
    return is_name_start(ch) || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
}
static int streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

// --- entity decode: tulis ke out (kapasitas cap), return panjang / -1 ---
static int decode_entity(Cur* c, char* out, int cap) {
    // c->p menunjuk '&'. Nama entity dibatasi 8 char; harus diakhiri ';'.
    const char* start = c->p;
    int start_col = c->col;
    bump(c);   // '&'
    char name[10];
    int n = 0;
    while (!at_end(c) && peek(c) != ';' && n < 9) { name[n++] = peek(c); bump(c); }
    if (at_end(c) || peek(c) != ';' || n == 0 || n >= 9) {
        set_error(c->err, BAD_ENTITY, c->line, start_col - (int)(c->p - start) + 1, 0, 0);
        return -1;
    }
    bump(c);   // ';'
    name[n] = '\0';
    const char* val = 0;
    if (streq(name, "lt")) val = "<";
    else if (streq(name, "gt")) val = ">";
    else if (streq(name, "amp")) val = "&";
    else if (streq(name, "quot")) val = "\"";
    else if (streq(name, "apos")) val = "'";
    else {
        set_error(c->err, BAD_ENTITY, c->line, start_col, 0, 0);
        return -1;
    }
    int vl = _ui_strlen(val);
    if (vl >= cap) return -1;   // kepanjangan ditangani pemanggil (ATTR_TOO_LONG)
    for (int i = 0; i < vl; i++) out[i] = val[i];
    return vl;
}

// --- nama tag/atribut (dialokasikan; caller own) ---
// Nama elemen DITRUNCATE ke MAX_ATTR_NAME (inflater menolaknya sebagai
// UNKNOWN_ELEMENT — deterministik). Nama atribut diperiksa panjangnya oleh
// pemanggil SEBELUM ke sini (ATTR_TOO_LONG); jadi truncate di sini hanya
// untuk nama elemen.
static char* parse_name(Cur* c) {
    if (!is_name_start(peek(c))) return 0;
    const char* s = c->p;
    while (is_name_char(peek(c))) bump(c);
    int n = (int)(c->p - s);
    if (n > MAX_ATTR_NAME) n = MAX_ATTR_NAME;
    char* out = (char*)_ui_alloc((unsigned)(n + 1));
    if (!out) return 0;
    for (int i = 0; i < n; i++) out[i] = s[i];
    out[n] = '\0';
    return out;
}

// --- nilai atribut ber-quote (sudah di dalam quote q); decode entity ---
static char* parse_attr_value(Cur* c, char q, const char* attr_name) {
    char* out = (char*)_ui_alloc((unsigned)(MAX_ATTR_VALUE + 1));
    if (!out) { set_error(c->err, OOM, c->line, c->col, 0, attr_name); return 0; }
    int n = 0;
    for (;;) {
        if (at_end(c)) {
            _ui_free(out);
            set_error(c->err, UNCLOSED, c->line, c->col, 0, attr_name);
            return 0;
        }
        char ch = peek(c);
        if (ch == q) { bump(c); break; }
        if (ch == '<') {
            _ui_free(out);
            set_error(c->err, SYNTAX, c->line, c->col, 0, attr_name);
            return 0;
        }
        if (ch == '&') {
            char tmp[4];
            int w = decode_entity(c, tmp, (int)sizeof(tmp));
            if (w < 0) { _ui_free(out); return 0; }   // error sudah di-set
            if (n + w > MAX_ATTR_VALUE) {
                _ui_free(out);
                set_error(c->err, ATTR_TOO_LONG, c->line, c->col, 0, attr_name);
                return 0;
            }
            for (int i = 0; i < w; i++) out[n++] = tmp[i];
            continue;
        }
        if (n >= MAX_ATTR_VALUE) {
            _ui_free(out);
            set_error(c->err, ATTR_TOO_LONG, c->line, c->col, 0, attr_name);
            return 0;
        }
        out[n++] = ch;
        bump(c);
    }
    out[n] = '\0';
    return out;
}

// --- komentar <!-- ... --> (tak bersarang; mentah, tanpa entity) ---
static int skip_comment(Cur* c) {
    // asumsi "<!--" sudah dikonsumsi
    for (;;) {
        if (at_end(c)) {
            set_error(c->err, SYNTAX, c->line, c->col, 0, 0);
            return 0;
        }
        if (peek(c) == '-' && c->p + 2 < c->end &&
            c->p[1] == '-' && c->p[2] == '>') {
            bump(c); bump(c); bump(c);
            return 1;
        }
        bump(c);
    }
}

// --- deklarasi/PI <? ... ?> (mentah sampai ?>) ---
static int skip_pi(Cur* c, int* is_decl) {
    // asumsi "<?" sudah dikonsumsi; baca target
    *is_decl = 0;
    const char* s = c->p;
    while (is_name_char(peek(c))) bump(c);
    int n = (int)(c->p - s);
    if (n == 3 && s[0] == 'x' && s[1] == 'm' && s[2] == 'l') *is_decl = 1;
    for (;;) {
        if (at_end(c)) {
            set_error(c->err, SYNTAX, c->line, c->col, 0, 0);
            return 0;
        }
        if (peek(c) == '?' && c->p + 1 < c->end && c->p[1] == '>') {
            bump(c); bump(c);
            return 1;
        }
        bump(c);
    }
}

static void free_node(Node* nd);

static Node* new_node(Cur* c, const char* name, int line, int col) {
    if (c->nnodes >= MAX_NODES) {
        set_error(c->err, TOO_MANY_NODES, line, col, name, 0);
        return 0;
    }
    Node* nd = new Node;
    if (!nd) { set_error(c->err, OOM, line, col, name, 0); return 0; }
    nd->name = _ui_strdup(name);
    nd->attrs = 0; nd->nattr = 0;
    nd->children = 0; nd->nchild = 0;
    nd->line = line; nd->column = col;
    if (!nd->name) { delete nd; set_error(c->err, OOM, line, col, name, 0); return 0; }
    c->nnodes++;
    return nd;
}

static void free_node(Node* nd) {
    if (!nd) return;
    _ui_free(nd->name);
    for (int i = 0; i < nd->nattr; i++) {
        _ui_free(nd->attrs[i].name);
        _ui_free(nd->attrs[i].value);
    }
    if (nd->attrs) delete[] nd->attrs;
    for (int i = 0; i < nd->nchild; i++) free_node(nd->children[i]);
    if (nd->children) delete[] nd->children;
    delete nd;
}

void free_doc(Document* doc) {
    if (!doc) return;
    free_node(doc->root);
    delete doc;
}

// Cari atribut duplikat di node yang sedang dibangun.
static int has_attr(Node* nd, const char* name) {
    for (int i = 0; i < nd->nattr; i++)
        if (streq(nd->attrs[i].name, name)) return 1;
    return 0;
}

// Parse SATU elemen buka (nama sudah dibaca) -> node terisi atribut;
// return: 1 = '>' (ada isi/tutup), 2 = '/>' (self-closing), 0 = gagal.
static int parse_open_tag(Cur* c, Node* nd) {
    Attr* arr = new Attr[MAX_ATTRS];
    if (!arr) { set_error(c->err, OOM, c->line, c->col, nd->name, 0); return 0; }
    nd->attrs = arr;
    for (;;) {
        skip_ws(c);
        if (at_end(c)) {
            set_error(c->err, UNCLOSED, c->line, c->col, nd->name, 0);
            return 0;
        }
        char ch = peek(c);
        if (ch == '>') { bump(c); return 1; }
        if (ch == '/') {
            // '/>' boleh berspasi (XML mengizinkan S sebelum '>')
            int sl = c->line, sc = c->col;
            bump(c);
            skip_ws(c);
            if (peek(c) != '>') {
                set_error(c->err, SYNTAX, sl, sc, nd->name, 0);
                return 0;
            }
            bump(c);
            return 2;
        }
        // atribut
        int an_line = c->line, an_col = c->col;
        if (!is_name_start(peek(c))) {
            set_error(c->err, SYNTAX, an_line, an_col, nd->name, 0);
            return 0;
        }
        {   // panjang nama dibatasi SEBELUM alokasi (ATTR_TOO_LONG deterministik)
            const char* s = c->p;
            int raw = 0;
            while (is_name_char(s[raw])) raw++;
            if (raw > MAX_ATTR_NAME) {
                char tmp[MAX_ATTR_NAME + 1];
                for (int i = 0; i < MAX_ATTR_NAME; i++) tmp[i] = s[i];
                tmp[MAX_ATTR_NAME] = '\0';
                set_error(c->err, ATTR_TOO_LONG, an_line, an_col, nd->name, tmp);
                return 0;
            }
        }
        char* aname = parse_name(c);
        if (!aname) {
            set_error(c->err, OOM, an_line, an_col, nd->name, 0);
            return 0;
        }
        if (nd->nattr >= MAX_ATTRS) {
            _ui_free(aname);
            set_error(c->err, TOO_MANY_ATTRS, an_line, an_col, nd->name, aname);
            return 0;
        }
        if (has_attr(nd, aname)) {
            set_error(c->err, SYNTAX, an_line, an_col, nd->name, aname);
            _ui_free(aname);
            return 0;   // atribut duplikat = syntax error deterministik
        }
        skip_ws(c);
        if (peek(c) != '=') {
            _ui_free(aname);
            set_error(c->err, SYNTAX, c->line, c->col, nd->name, 0);
            return 0;
        }
        bump(c);
        skip_ws(c);
        char q = peek(c);
        if (q != '"' && q != '\'') {
            _ui_free(aname);
            set_error(c->err, SYNTAX, c->line, c->col, nd->name, aname);
            return 0;
        }
        bump(c);
        char* aval = parse_attr_value(c, q, aname);
        if (!aval) { _ui_free(aname); return 0; }
        nd->attrs[nd->nattr].name = aname;
        nd->attrs[nd->nattr].value = aval;
        nd->nattr++;
    }
}

// Parse isi elemen (anak + tutup). depth = kedalaman elemen INI.
static int parse_content(Cur* c, Node* nd, int depth) {
    Node** arr = new Node*[MAX_CHILDREN];
    if (!arr) { set_error(c->err, OOM, c->line, c->col, nd->name, 0); return 0; }
    nd->children = arr;
    for (;;) {
        skip_ws(c);
        if (at_end(c)) {
            set_error(c->err, UNCLOSED, nd->line, nd->column, nd->name, 0);
            return 0;
        }
        if (peek(c) != '<') {
            // teks: hanya whitespace yang boleh (sudah skip) -> apa pun di
            // sini non-ws. Entity di teks tak didukung (atribut saja).
            set_error(c->err, BAD_TEXT, c->line, c->col, nd->name, 0);
            return 0;
        }
        // '<' ...
        int lt_line = c->line, lt_col = c->col;
        bump(c);
        if (at_end(c)) {
            set_error(c->err, UNCLOSED, lt_line, lt_col, nd->name, 0);
            return 0;
        }
        char ch = peek(c);
        if (ch == '/') {
            // tutup
            bump(c);
            char* cname = parse_name(c);
            if (!cname) {
                set_error(c->err, SYNTAX, c->line, c->col, nd->name, 0);
                return 0;
            }
            int ok = streq(cname, nd->name);
            skip_ws(c);
            if (peek(c) != '>') {
                _ui_free(cname);
                set_error(c->err, SYNTAX, c->line, c->col, nd->name, 0);
                return 0;
            }
            bump(c);
            if (!ok) {
                set_error(c->err, MISMATCH, lt_line, lt_col, cname, 0);
                _ui_free(cname);
                return 0;
            }
            _ui_free(cname);
            return 1;
        }
        if (ch == '!') {
            bump(c);
            if (c->p + 1 < c->end && c->p[0] == '-' && c->p[1] == '-') {
                bump(c); bump(c);
                if (!skip_comment(c)) return 0;
                continue;
            }
            if (c->p + 6 < c->end && c->p[0] == '[' && c->p[1] == 'C' &&
                c->p[2] == 'D' && c->p[3] == 'A' && c->p[4] == 'T' &&
                c->p[5] == 'A' && c->p[6] == '[') {
                set_error(c->err, UNSUPPORTED, lt_line, lt_col, nd->name, 0);
                return 0;
            }
            set_error(c->err, UNSUPPORTED, lt_line, lt_col, nd->name, 0);
            return 0;   // DOCTYPE / markup lain = di luar subset
        }
        if (ch == '?') {
            set_error(c->err, UNSUPPORTED, lt_line, lt_col, nd->name, 0);
            return 0;   // PI hanya legal sebelum akar
        }
        // elemen anak
        if (!is_name_start(ch)) {
            set_error(c->err, SYNTAX, c->line, c->col, nd->name, 0);
            return 0;
        }
        if (depth + 1 > MAX_DEPTH) {
            set_error(c->err, TOO_DEEP, lt_line, lt_col, nd->name, 0);
            return 0;
        }
        if (nd->nchild >= MAX_CHILDREN) {
            set_error(c->err, TOO_MANY_CHILDREN, lt_line, lt_col, nd->name, 0);
            return 0;
        }
        char* cname = parse_name(c);
        if (!cname) {
            set_error(c->err, SYNTAX, c->line, c->col, nd->name, 0);
            return 0;
        }
        Node* child = new_node(c, cname, lt_line, lt_col);
        _ui_free(cname);
        if (!child) return 0;
        int kind = parse_open_tag(c, child);
        if (kind == 0) { free_node(child); return 0; }
        if (kind == 1 && !parse_content(c, child, depth + 1)) {
            free_node(child);
            return 0;
        }
        nd->children[nd->nchild++] = child;
    }
}

Document* parse(const char* data, unsigned size, Error* err) {
    Error local;
    if (!err) err = &local;
    set_error(err, OK, 0, 0, 0, 0);
    if (!data || size == 0 || size > (unsigned)MAX_DOC) {
        if (!data || size == 0) set_error(err, EMPTY, 0, 0, 0, 0);
        else set_error(err, TOO_BIG, 1, 1, 0, 0);
        return 0;
    }
    Cur c;
    c.p = data; c.end = data + size;
    c.line = 1; c.col = 1; c.nnodes = 0; c.err = err;

    // prolog: ws + deklarasi/PI/komentar sebelum akar
    int seen_pi = 0;
    for (;;) {
        skip_ws(&c);
        if (at_end(&c)) { set_error(err, EMPTY, 0, 0, 0, 0); return 0; }
        if (peek(&c) != '<') { set_error(err, BAD_TEXT, c.line, c.col, 0, 0); return 0; }
        if (c.p + 1 < c.end && c.p[1] == '?') {
            int pl = c.line, pc = c.col;
            bump(&c); bump(&c);
            int is_decl = 0;
            if (!skip_pi(&c, &is_decl)) return 0;
            if (!is_decl) { set_error(err, UNSUPPORTED, pl, pc, 0, 0); return 0; }
            if (seen_pi) { set_error(err, SYNTAX, pl, pc, 0, 0); return 0; }
            seen_pi = 1;
            continue;
        }
        if (c.p + 3 < c.end && c.p[1] == '!' && c.p[2] == '-' && c.p[3] == '-') {
            bump(&c); bump(&c); bump(&c); bump(&c);
            if (!skip_comment(&c)) return 0;
            continue;
        }
        if (c.p + 1 < c.end && c.p[1] == '!') {
            // DOCTYPE/markup deklaratif di prolog = di luar subset
            set_error(err, UNSUPPORTED, c.line, c.col, 0, 0);
            return 0;
        }
        break;   // '<' elemen akar (atau markup ilegal -> error di bawah)
    }

    if (peek(&c) != '<' || !is_name_start(c.p + 1 < c.end ? c.p[1] : '\0')) {
        set_error(err, SYNTAX, c.line, c.col, 0, 0);
        return 0;
    }
    int rl = c.line, rc = c.col;
    bump(&c);
    char* rname = parse_name(&c);
    if (!rname) { set_error(err, SYNTAX, rl, rc, 0, 0); return 0; }
    Node* root = new_node(&c, rname, rl, rc);
    _ui_free(rname);
    if (!root) return 0;
    int kind = parse_open_tag(&c, root);
    if (kind == 0) { free_node(root); return 0; }
    if (kind == 1 && !parse_content(&c, root, 1)) { free_node(root); return 0; }

    // epilog: hanya ws/komentar; elemen kedua = MULTI_ROOT, teks = BAD_TEXT
    for (;;) {
        skip_ws(&c);
        if (at_end(&c)) break;
        if (peek(&c) != '<') { set_error(err, BAD_TEXT, c.line, c.col, 0, 0); free_node(root); return 0; }
        if (c.p + 3 < c.end && c.p[1] == '!' && c.p[2] == '-' && c.p[3] == '-') {
            bump(&c); bump(&c); bump(&c); bump(&c);
            if (!skip_comment(&c)) { free_node(root); return 0; }
            continue;
        }
        set_error(err, MULTI_ROOT, c.line, c.col, 0, 0);
        free_node(root);
        return 0;
    }

    Document* doc = new Document;
    if (!doc) { set_error(err, OOM, 0, 0, 0, 0); free_node(root); return 0; }
    doc->root = root;
    set_error(err, OK, 0, 0, 0, 0);
    return doc;
}

} // namespace xml
} // namespace ui
