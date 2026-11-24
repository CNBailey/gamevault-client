#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int depth; } parser_t;

static void skip_ws(parser_t *ps) { while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++; }

static jval_t *new_val(jtype_t t) {
  jval_t *v = calloc(1, sizeof *v);
  if (v) v->type = t;
  return v;
}

static void put_utf8(sbuf_t *b, unsigned cp) {
  char out[4];
  if (cp < 0x80) { out[0] = (char)cp; sbuf_append(b, out, 1); }
  else if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); sbuf_append(b, out, 2); }
  else if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); sbuf_append(b, out, 3); }
  else { out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); sbuf_append(b, out, 4); }
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}

static char *parse_string(parser_t *ps) {
  if (*ps->p != '"') return NULL;
  ps->p++;
  sbuf_t b;
  sbuf_init(&b);
  while (*ps->p && *ps->p != '"') {
    char c = *ps->p++;
    if (c != '\\') { sbuf_append(&b, &c, 1); continue; }
    char e = *ps->p++;
    switch (e) {
      case '"': case '\\': case '/': sbuf_append(&b, &e, 1); break;
      case 'b': sbuf_append(&b, "\b", 1); break;
      case 'f': sbuf_append(&b, "\f", 1); break;
      case 'n': sbuf_append(&b, "\n", 1); break;
      case 'r': sbuf_append(&b, "\r", 1); break;
      case 't': sbuf_append(&b, "\t", 1); break;
      case 'u': {
        unsigned cp;
        if (hex4(ps->p, &cp)) { sbuf_free(&b); return NULL; }
        ps->p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
          unsigned lo;
          if (!hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            ps->p += 6;
          }
        }
        put_utf8(&b, cp);
        break;
      }
      default: sbuf_free(&b); return NULL;
    }
  }
  if (*ps->p != '"') { sbuf_free(&b); return NULL; }
  ps->p++;
  return sbuf_detach(&b);
}

static jval_t *parse_value(parser_t *ps);

static jval_t *parse_container(parser_t *ps, bool obj) {
  jval_t *v = new_val(obj ? J_OBJ : J_ARR);
  if (!v) return NULL;
  ps->p++;
  if (++ps->depth > 64) { json_free(v); return NULL; }
  jval_t **tail = &v->child;
  skip_ws(ps);
  if (*ps->p == (obj ? '}' : ']')) { ps->p++; ps->depth--; return v; }
  for (;;) {
    char *key = NULL;
    skip_ws(ps);
    if (obj) {
      key = parse_string(ps);
      if (!key) { json_free(v); return NULL; }
      skip_ws(ps);
      if (*ps->p != ':') { free(key); json_free(v); return NULL; }
      ps->p++;
    }
    jval_t *c = parse_value(ps);
    if (!c) { free(key); json_free(v); return NULL; }
    c->key = key;
    *tail = c;
    tail = &c->next;
    skip_ws(ps);
    if (*ps->p == ',') { ps->p++; continue; }
    if (*ps->p == (obj ? '}' : ']')) { ps->p++; break; }
    json_free(v);
    return NULL;
  }
  ps->depth--;
  return v;
}

static jval_t *parse_value(parser_t *ps) {
  skip_ws(ps);
  char c = *ps->p;
  if (c == '{') return parse_container(ps, true);
  if (c == '[') return parse_container(ps, false);
  if (c == '"') {
    char *s = parse_string(ps);
    if (!s) return NULL;
    jval_t *v = new_val(J_STR);
    v->s = s;
    return v;
  }
  if (!strncmp(ps->p, "true", 4)) { ps->p += 4; jval_t *v = new_val(J_BOOL); v->b = true; return v; }
  if (!strncmp(ps->p, "false", 5)) { ps->p += 5; jval_t *v = new_val(J_BOOL); v->b = false; return v; }
  if (!strncmp(ps->p, "null", 4)) { ps->p += 4; return new_val(J_NULL); }
  if (c == '-' || isdigit((unsigned char)c)) {
    char *end;
    double n = strtod(ps->p, &end);
    if (end == ps->p) return NULL;
    ps->p = end;
    jval_t *v = new_val(J_NUM);
    v->n = n;
    return v;
  }
  return NULL;
}

jval_t *json_parse(const char *text) {
  if (!text) return NULL;
  parser_t ps = { text, 0 };
  jval_t *v = parse_value(&ps);
  if (!v) return NULL;
  skip_ws(&ps);
  if (*ps.p) { json_free(v); return NULL; }
  return v;
}

void json_free(jval_t *v) {
  while (v) {
    jval_t *next = v->next;
    json_free(v->child);
    free(v->s);
    free(v->key);
    free(v);
    v = next;
  }
}

jval_t *json_get(const jval_t *obj, const char *key) {
  if (!obj || obj->type != J_OBJ) return NULL;
  for (jval_t *c = obj->child; c; c = c->next) {
    if (c->key && !strcmp(c->key, key)) return c;
  }
  return NULL;
}

const char *json_str(const jval_t *obj, const char *key, const char *fallback) {
  jval_t *v = json_get(obj, key);
  return v && v->type == J_STR ? v->s : fallback;
}

double json_num(const jval_t *obj, const char *key, double fallback) {
  jval_t *v = json_get(obj, key);
  return v && v->type == J_NUM ? v->n : fallback;
}

bool json_bool(const jval_t *obj, const char *key, bool fallback) {
  jval_t *v = json_get(obj, key);
  return v && v->type == J_BOOL ? v->b : fallback;
}

void jw_str(sbuf_t *b, const char *s) {
  if (!s) { sbuf_puts(b, "null"); return; }
  sbuf_append(b, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"': sbuf_puts(b, "\\\""); break;
      case '\\': sbuf_puts(b, "\\\\"); break;
      case '\n': sbuf_puts(b, "\\n"); break;
      case '\r': sbuf_puts(b, "\\r"); break;
      case '\t': sbuf_puts(b, "\\t"); break;
      default:
        if (*p < 0x20) sbuf_printf(b, "\\u%04x", *p);
        else sbuf_append(b, p, 1);
    }
  }
  sbuf_append(b, "\"", 1);
}

static void key(sbuf_t *b, const char *k, bool comma) {
  if (comma) sbuf_append(b, ",", 1);
  jw_str(b, k);
  sbuf_append(b, ":", 1);
}

void jw_kv_str(sbuf_t *b, const char *k, const char *v, bool comma) { key(b, k, comma); jw_str(b, v); }
void jw_kv_num(sbuf_t *b, const char *k, double v, bool comma) { key(b, k, comma); sbuf_printf(b, "%.3f", v); }
void jw_kv_int(sbuf_t *b, const char *k, int64_t v, bool comma) { key(b, k, comma); sbuf_printf(b, "%lld", (long long)v); }
void jw_kv_bool(sbuf_t *b, const char *k, bool v, bool comma) { key(b, k, comma); sbuf_puts(b, v ? "true" : "false"); }
