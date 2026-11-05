#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_NULL, T_FALSE, T_TRUE, T_NUM, T_STR, T_ARR, T_OBJ };

struct json_t {
  int type;
  double num;
  char *str;
  json_t **items;
  char **keys;
  size_t n, cap;
};

typedef struct {
  const char *p;
  int depth;
} ps;

static json_t *pval(ps *s);

static json_t *mk(int t) {
  json_t *j = calloc(1, sizeof *j);
  if (j) j->type = t;
  return j;
}

void json_decref(json_t *j) {
  if (!j) return;
  free(j->str);
  for (size_t i = 0; i < j->n; i++) {
    json_decref(j->items[i]);
    if (j->keys) free(j->keys[i]);
  }
  free(j->items);
  free(j->keys);
  free(j);
}

static int add(json_t *c, char *key, json_t *v) {
  if (c->n == c->cap) {
    size_t nc = c->cap ? c->cap * 2 : 8;
    json_t **ni = realloc(c->items, nc * sizeof *ni);
    if (!ni) return 0;
    c->items = ni;
    if (c->type == T_OBJ) {
      char **nk = realloc(c->keys, nc * sizeof *nk);
      if (!nk) return 0;
      c->keys = nk;
    }
    c->cap = nc;
  }
  c->items[c->n] = v;
  if (c->type == T_OBJ) c->keys[c->n] = key;
  c->n++;
  return 1;
}

static void ws(ps *s) {
  while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++;
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

static char *pstr(ps *s) {
  s->p++; /* opening quote */
  size_t cap = 32, n = 0;
  char *o = malloc(cap);
  if (!o) return NULL;
  for (;;) {
    unsigned char c = (unsigned char)*s->p;
    if (!c) goto bad;
    if (c == '"') {
      s->p++;
      break;
    }
    if (n + 8 > cap) {
      cap *= 2;
      char *t = realloc(o, cap);
      if (!t) goto bad;
      o = t;
    }
    if (c == '\\') {
      s->p++;
      switch (*s->p) {
        case '"': o[n++] = '"'; break;
        case '\\': o[n++] = '\\'; break;
        case '/': o[n++] = '/'; break;
        case 'b': o[n++] = '\b'; break;
        case 'f': o[n++] = '\f'; break;
        case 'n': o[n++] = '\n'; break;
        case 'r': o[n++] = '\r'; break;
        case 't': o[n++] = '\t'; break;
        case 'u': {
          unsigned cp, lo;
          if (!hex4(s->p + 1, &cp)) goto bad;
          s->p += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && s->p[1] == '\\' && s->p[2] == 'u' &&
              hex4(s->p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            s->p += 6;
          }
          if (cp < 0x80) o[n++] = (char)cp;
          else if (cp < 0x800) {
            o[n++] = (char)(0xC0 | (cp >> 6));
            o[n++] = (char)(0x80 | (cp & 0x3F));
          } else if (cp < 0x10000) {
            o[n++] = (char)(0xE0 | (cp >> 12));
            o[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            o[n++] = (char)(0x80 | (cp & 0x3F));
          } else {
            o[n++] = (char)(0xF0 | (cp >> 18));
            o[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            o[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            o[n++] = (char)(0x80 | (cp & 0x3F));
          }
          break;
        }
        default: goto bad;
      }
      s->p++;
    } else {
      o[n++] = (char)c;
      s->p++;
    }
  }
  o[n] = 0;
  return o;
bad:
  free(o);
  return NULL;
}

static json_t *pcontainer(ps *s, int type) {
  char close = type == T_OBJ ? '}' : ']';
  s->p++;
  json_t *o = mk(type);
  if (!o) return NULL;
  s->depth++;
  ws(s);
  if (*s->p == close) {
    s->p++;
    s->depth--;
    return o;
  }
  for (;;) {
    char *key = NULL;
    ws(s);
    if (type == T_OBJ) {
      if (*s->p != '"') goto bad;
      key = pstr(s);
      if (!key) goto bad;
      ws(s);
      if (*s->p != ':') {
        free(key);
        goto bad;
      }
      s->p++;
    }
    json_t *v = pval(s);
    if (!v) {
      free(key);
      goto bad;
    }
    if (!add(o, key, v)) {
      free(key);
      json_decref(v);
      goto bad;
    }
    ws(s);
    if (*s->p == ',') {
      s->p++;
      continue;
    }
    if (*s->p == close) {
      s->p++;
      break;
    }
    goto bad;
  }
  s->depth--;
  return o;
bad:
  json_decref(o);
  return NULL;
}

static json_t *pval(ps *s) {
  ws(s);
  if (s->depth > 64) return NULL;
  char c = *s->p;
  if (c == '{') return pcontainer(s, T_OBJ);
  if (c == '[') return pcontainer(s, T_ARR);
  if (c == '"') {
    char *str = pstr(s);
    if (!str) return NULL;
    json_t *j = mk(T_STR);
    if (!j) {
      free(str);
      return NULL;
    }
    j->str = str;
    return j;
  }
  if (!strncmp(s->p, "true", 4)) {
    s->p += 4;
    return mk(T_TRUE);
  }
  if (!strncmp(s->p, "false", 5)) {
    s->p += 5;
    return mk(T_FALSE);
  }
  if (!strncmp(s->p, "null", 4)) {
    s->p += 4;
    return mk(T_NULL);
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    char *end;
    double d = strtod(s->p, &end);
    if (end == s->p) return NULL;
    s->p = end;
    json_t *j = mk(T_NUM);
    if (j) j->num = d;
    return j;
  }
  return NULL;
}

json_t *json_loads(const char *str, size_t flags, void *err) {
  (void)flags;
  (void)err;
  if (!str) return NULL;
  ps s = {str, 0};
  json_t *j = pval(&s);
  return j;
}

json_t *json_object_get(const json_t *o, const char *key) {
  if (!o || o->type != T_OBJ) return NULL;
  for (size_t i = 0; i < o->n; i++)
    if (!strcmp(o->keys[i], key)) return o->items[i];
  return NULL;
}

json_t *json_array_get(const json_t *a, size_t i) {
  if (!a || a->type != T_ARR || i >= a->n) return NULL;
  return a->items[i];
}

size_t json_array_size(const json_t *a) { return a && a->type == T_ARR ? a->n : 0; }
int json_is_string(const json_t *j) { return j && j->type == T_STR; }
int json_is_number(const json_t *j) { return j && j->type == T_NUM; }
int json_is_true(const json_t *j) { return j && j->type == T_TRUE; }
int json_is_array(const json_t *j) { return j && j->type == T_ARR; }
int json_is_object(const json_t *j) { return j && j->type == T_OBJ; }
const char *json_string_value(const json_t *j) { return json_is_string(j) ? j->str : NULL; }
double json_number_value(const json_t *j) { return json_is_number(j) ? j->num : 0; }

size_t json_escape(const char *in, char *out, size_t n) {
  size_t o = 0;
  if (!n) return 0;
  for (; in && *in && o + 7 < n; in++) {
    unsigned char c = (unsigned char)*in;
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if (c < 0x20) {
      o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = 0;
  return o;
}
