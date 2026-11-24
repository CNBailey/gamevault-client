/* Minimal JSON reader (tree) and writer helpers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "util.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jval {
  jtype_t type;
  bool b;
  double n;
  char *s;            /* J_STR */
  char *key;          /* when inside an object */
  struct jval *child; /* first child for J_ARR / J_OBJ */
  struct jval *next;  /* next sibling */
} jval_t;

jval_t *json_parse(const char *text);       /* NULL on error */
void json_free(jval_t *v);
jval_t *json_get(const jval_t *obj, const char *key);
const char *json_str(const jval_t *obj, const char *key, const char *fallback);
double json_num(const jval_t *obj, const char *key, double fallback);
bool json_bool(const jval_t *obj, const char *key, bool fallback);

/* Writer: appends to an sbuf. Caller manages commas with jw_* helpers. */
void jw_str(sbuf_t *b, const char *s);                 /* quoted, escaped; NULL -> null */
void jw_kv_str(sbuf_t *b, const char *k, const char *v, bool comma);
void jw_kv_num(sbuf_t *b, const char *k, double v, bool comma);
void jw_kv_int(sbuf_t *b, const char *k, int64_t v, bool comma);
void jw_kv_bool(sbuf_t *b, const char *k, bool v, bool comma);
