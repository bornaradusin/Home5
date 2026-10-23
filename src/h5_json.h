/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Home5: a small JSON reader for Home Assistant's messages. The text is
 * split into tokens once; values are then looked up by key and position.
 * (Taken from PC5.) Plus a writer's string escaping. */
#ifndef H5_JSON_H
#define H5_JSON_H
#include "h5.h"

typedef enum H5JsonType { H5_JSON_NONE, H5_JSON_OBJECT, H5_JSON_ARRAY, H5_JSON_STRING, H5_JSON_NUMBER, H5_JSON_LITERAL } H5JsonType;

typedef struct H5JsonToken {
    H5JsonType type;
    uint32_t start, end;     /* the text it covers (a string's without its quotes) */
    uint32_t children;       /* an object's keys plus values, an array's items */
    uint32_t next;           /* the token after it and everything inside it */
} H5JsonToken;

typedef struct H5Json {
    const char *text;
    H5JsonToken *tokens;
    uint32_t count;
} H5Json;

/* 0, or -1 for text that is not JSON (or too big: max_tokens). */
int h5_json_parse(H5Json *json, const char *text, size_t length, uint32_t max_tokens);
void h5_json_free(H5Json *json);
/* An object's value for key, an array's item; 0 when there is none (token 0
 * is the whole document, so it is never a child). */
uint32_t h5_json_get(const H5Json *json, uint32_t object, const char *key);
uint32_t h5_json_item(const H5Json *json, uint32_t array, uint32_t index);
/* A string's text, unescaped to UTF-8; a number's or literal's text as is. */
int h5_json_text(const H5Json *json, uint32_t token, char *out, size_t size);
/* A number (or a string holding one); fallback when it is neither. */
double h5_json_number(const H5Json *json, uint32_t token, double fallback);
/* Whether a token is the literal true. */
int h5_json_true(const H5Json *json, uint32_t token);
/* An array's or object's first child and the one after a child: for walking
 * big arrays without counting from the start each time. */
static inline uint32_t h5_json_first(const H5Json *json, uint32_t token)
{
    return token + 1 < json->count ? token + 1 : 0;
}
static inline uint32_t h5_json_next(const H5Json *json, uint32_t token)
{
    return json->tokens[token].next;
}

/* text as a JSON string, quotes included, appended to out. 0 or -1. */
int h5_json_quote(H5Buf *out, const char *text);

#endif
