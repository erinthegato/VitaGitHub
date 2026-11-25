// json.c - a tiny recursive JSON parser (no external libraries needed).
#include <stdlib.h>
#include <string.h>
#include "json.h"

#define MAX_DEPTH 64

typedef struct { const char *p; } Parser;

static void skip_ws(Parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    *out = v;
    return 1;
}

// Write a Unicode code point as UTF-8. Returns number of bytes written.
static int put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) {
        o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F);
        o[2] = 0x80 | (cp & 0x3F); return 3;
    }
    o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 0x3F);
    o[2] = 0x80 | ((cp >> 6) & 0x3F); o[3] = 0x80 | (cp & 0x3F); return 4;
}

// Parses a "string". ps->p must point at the opening quote.
static char *parse_string(Parser *ps) {
    const char *s = ps->p + 1, *e = s;
    // Find the closing quote first so we know how much memory we need.
    while (*e && *e != '"') {
        if (*e == '\\') { e++; if (!*e) return NULL; }
        e++;
    }
    if (*e != '"') return NULL;

    char *out = malloc((e - s) + 1);   // decoded text is never longer
    if (!out) return NULL;
    char *o = out;
    const char *p = s;
    while (p < e) {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; p++; break;
        case 't': *o++ = '\t'; p++; break;
        case 'r': *o++ = '\r'; p++; break;
        case 'b': *o++ = '\b'; p++; break;
        case 'f': *o++ = '\f'; p++; break;
        case 'u': {
            unsigned cp;
            if (e - p < 5 || !hex4(p + 1, &cp)) { free(out); return NULL; }
            p += 5;
            // Emoji etc. come as two \u escapes (a "surrogate pair").
            if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                unsigned lo;
                if (hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            o += put_utf8(o, cp);
            break;
        }
        default: *o++ = *p++; break;   // \" \\ \/
        }
    }
    *o = 0;
    ps->p = e + 1;
    return out;
}

static JVal *new_val(int type) {
    JVal *v = calloc(1, sizeof(JVal));
    if (v) v->type = type;
    return v;
}

static JVal *parse_value(Parser *ps, int depth);

// Shared code for arrays [..] and objects {..}
static JVal *parse_container(Parser *ps, int depth, int is_obj) {
    char close = is_obj ? '}' : ']';
    JVal *v = new_val(is_obj ? J_OBJ : J_ARR), *tail = NULL;
    if (!v) return NULL;
    ps->p++;  // skip [ or {
    skip_ws(ps);
    if (*ps->p == close) { ps->p++; return v; }

    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (is_obj) {
            if (*ps->p != '"' || !(key = parse_string(ps))) goto fail;
            skip_ws(ps);
            if (*ps->p != ':') { free(key); goto fail; }
            ps->p++;
        }
        JVal *item = parse_value(ps, depth + 1);
        if (!item) { free(key); goto fail; }
        item->key = key;
        if (tail) tail->next = item; else v->child = item;
        tail = item;

        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; return v; }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static JVal *parse_value(Parser *ps, int depth) {
    if (depth > MAX_DEPTH) return NULL;
    skip_ws(ps);
    char c = *ps->p;
    if (c == '{') return parse_container(ps, depth, 1);
    if (c == '[') return parse_container(ps, depth, 0);
    if (c == '"') {
        char *s = parse_string(ps);
        if (!s) return NULL;
        JVal *v = new_val(J_STR);
        if (!v) { free(s); return NULL; }
        v->str = s;
        return v;
    }
    if (!strncmp(ps->p, "true", 4))  { ps->p += 4; JVal *v = new_val(J_BOOL); if (v) v->boolean = 1; return v; }
    if (!strncmp(ps->p, "false", 5)) { ps->p += 5; return new_val(J_BOOL); }
    if (!strncmp(ps->p, "null", 4))  { ps->p += 4; return new_val(J_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(ps->p, &end);
        if (end == ps->p) return NULL;
        ps->p = end;
        JVal *v = new_val(J_NUM);
        if (v) v->num = d;
        return v;
    }
    return NULL;
}

JVal *json_parse(const char *text) {
    if (!text) return NULL;
    Parser ps = { text };
    JVal *v = parse_value(&ps, 0);
    if (!v) return NULL;
    skip_ws(&ps);
    if (*ps.p) { json_free(v); return NULL; }   // junk after the JSON
    return v;
}

void json_free(JVal *v) {
    while (v) {
        JVal *next = v->next;
        json_free(v->child);
        free(v->key);
        free(v->str);
        free(v);
        v = next;
    }
}

JVal *json_get(JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (JVal *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const char *json_str(JVal *obj, const char *key, const char *def) {
    JVal *v = json_get(obj, key);
    return (v && v->type == J_STR) ? v->str : def;
}

double json_num(JVal *obj, const char *key, double def) {
    JVal *v = json_get(obj, key);
    return (v && v->type == J_NUM) ? v->num : def;
}

int json_bool(JVal *obj, const char *key, int def) {
    JVal *v = json_get(obj, key);
    return (v && v->type == J_BOOL) ? v->boolean : def;
}

int json_count(JVal *arr) {
    int n = 0;
    if (arr && (arr->type == J_ARR || arr->type == J_OBJ))
        for (JVal *c = arr->child; c; c = c->next) n++;
    return n;
}
