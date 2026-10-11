#ifndef DMM_IDE_JSON_H
#define DMM_IDE_JSON_H
#include <stdint.h>
#include <stdio.h>
typedef enum { JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_BOOL, JSON_NULL } JsonKind;
typedef struct JsonValue {
    JsonKind kind;
    char *name, *text;
    struct JsonValue *child, *next;
} JsonValue;
JsonValue *ide_json_parse(const char *text);
void ide_json_free(JsonValue *value);
const JsonValue *ide_json_get(const JsonValue *object, const char *name);
const char *ide_json_string(const JsonValue *object, const char *name);
int ide_json_integer(const JsonValue *value, int64_t *result);
void ide_json_quote(FILE *output, const char *text);
#endif
