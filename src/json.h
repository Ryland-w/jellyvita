#pragma once
#include <stddef.h>

/* Minimal JSON reader (replaces jansson, whose prebuilt Vita library breaks vita-elf-create). */
typedef struct json_t json_t;

json_t *json_loads(const char *s, size_t flags, void *err);
void json_decref(json_t *j); /* frees the whole tree; call on the root only */

json_t *json_object_get(const json_t *o, const char *key);
json_t *json_array_get(const json_t *a, size_t i);
size_t json_array_size(const json_t *a);

int json_is_string(const json_t *j);
int json_is_number(const json_t *j);
int json_is_true(const json_t *j);
int json_is_array(const json_t *j);
int json_is_object(const json_t *j);

const char *json_string_value(const json_t *j);
double json_number_value(const json_t *j);

#define json_array_foreach(a, i, v) \
  for ((i) = 0; (i) < json_array_size(a) && ((v) = json_array_get((a), (i))); (i)++)

/* Writes a JSON-escaped copy of `in` (no quotes) into out; returns length written. */
size_t json_escape(const char *in, char *out, size_t n);
