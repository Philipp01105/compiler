#include "ide_json.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cursor;
    size_t nodes;
} Parser;
static void space(Parser *p) {
    while (*p->cursor == ' ' || *p->cursor == '\t' || *p->cursor == '\r' || *p->cursor == '\n')
        p->cursor++;
}
void ide_json_free(JsonValue *v) {
    while (v) {
        JsonValue *next = v->next;
        ide_json_free(v->child);
        free(v->name);
        free(v->text);
        free(v);
        v = next;
    }
}
static int hex(unsigned char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
static int codepoint(Parser *p, unsigned *result) {
    unsigned n = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hex((unsigned char)*p->cursor);
        if (digit < 0)
            return 0;
        n = n * 16 + (unsigned)digit;
        p->cursor++;
    }
    *result = n;
    return 1;
}
static char *string(Parser *p) {
    if (*p->cursor++ != '"')
        return NULL;
    size_t capacity = 32, length = 0;
    char *result = malloc(capacity);
    if (!result)
        return NULL;
    while (*p->cursor && *p->cursor != '"') {
        unsigned c = (unsigned char)*p->cursor++;
        char bytes[4];
        size_t count = 1;
        if (c < 32)
            goto fail;
        if (c == '\\') {
            c = (unsigned char)*p->cursor++;
            switch (c) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u': {
                if (!codepoint(p, &c) || c == 0)
                    goto fail;
                if (c >= 0xd800 && c <= 0xdbff) {
                    unsigned low;
                    if (p->cursor[0] != '\\' || p->cursor[1] != 'u')
                        goto fail;
                    p->cursor += 2;
                    if (!codepoint(p, &low) || low < 0xdc00 || low > 0xdfff)
                        goto fail;
                    c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff)
                    goto fail;
                if (c < 0x80)
                    break;
                if (c < 0x800) {
                    bytes[0] = (char)(0xc0 | (c >> 6));
                    bytes[1] = (char)(0x80 | (c & 63));
                    count = 2;
                } else if (c < 0x10000) {
                    bytes[0] = (char)(0xe0 | (c >> 12));
                    bytes[1] = (char)(0x80 | ((c >> 6) & 63));
                    bytes[2] = (char)(0x80 | (c & 63));
                    count = 3;
                } else {
                    bytes[0] = (char)(0xf0 | (c >> 18));
                    bytes[1] = (char)(0x80 | ((c >> 12) & 63));
                    bytes[2] = (char)(0x80 | ((c >> 6) & 63));
                    bytes[3] = (char)(0x80 | (c & 63));
                    count = 4;
                }
                break;
            }
            default:
                goto fail;
            }
        }
        if (count == 1)
            bytes[0] = (char)c;
        if (length + count + 1 > capacity) {
            capacity *= 2;
            char *grown = realloc(result, capacity);
            if (!grown)
                goto fail;
            result = grown;
        }
        memcpy(result + length, bytes, count);
        length += count;
    }
    if (*p->cursor != '"')
        goto fail;
    p->cursor++;
    result[length] = 0;
    return result;
fail:
    free(result);
    return NULL;
}
static JsonValue *value(Parser *p, unsigned depth) {
    space(p);
    if (depth > 64 || ++p->nodes > 200000)
        return NULL;
    JsonValue *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    char opening = *p->cursor;
    if (opening == '{' || opening == '[') {
        v->kind = opening == '{' ? JSON_OBJECT : JSON_ARRAY;
        char closing = opening == '{' ? '}' : ']';
        p->cursor++;
        space(p);
        JsonValue **tail = &v->child;
        if (*p->cursor == closing) {
            p->cursor++;
            return v;
        }
        while (1) {
            char *name = NULL;
            if (opening == '{') {
                if (*p->cursor != '"' || !(name = string(p)))
                    goto fail;
                space(p);
                if (*p->cursor++ != ':') {
                    free(name);
                    goto fail;
                }
                for (JsonValue *child = v->child; child; child = child->next)
                    if (!strcmp(child->name, name)) {
                        free(name);
                        goto fail;
                    }
            }
            JsonValue *child = value(p, depth + 1);
            if (!child) {
                free(name);
                goto fail;
            }
            child->name = name;
            *tail = child;
            tail = &child->next;
            space(p);
            if (*p->cursor == closing) {
                p->cursor++;
                return v;
            }
            if (*p->cursor++ != ',')
                goto fail;
            space(p);
        }
    }
    if (opening == '"') {
        v->kind = JSON_STRING;
        v->text = string(p);
        if (!v->text)
            goto fail;
        return v;
    }
    const char *start = p->cursor;
    if (opening == '-' || isdigit((unsigned char)opening)) {
        v->kind = JSON_NUMBER;
        if (*p->cursor == '-')
            p->cursor++;
        if (*p->cursor == '0')
            p->cursor++;
        else {
            if (*p->cursor < '1' || *p->cursor > '9')
                goto fail;
            while (isdigit((unsigned char)*p->cursor))
                p->cursor++;
        }
        if (*p->cursor == '.') {
            p->cursor++;
            if (!isdigit((unsigned char)*p->cursor))
                goto fail;
            while (isdigit((unsigned char)*p->cursor))
                p->cursor++;
        }
        if (*p->cursor == 'e' || *p->cursor == 'E') {
            p->cursor++;
            if (*p->cursor == '+' || *p->cursor == '-')
                p->cursor++;
            if (!isdigit((unsigned char)*p->cursor))
                goto fail;
            while (isdigit((unsigned char)*p->cursor))
                p->cursor++;
        }
    } else if (!strncmp(start, "true", 4)) {
        v->kind = JSON_BOOL;
        p->cursor += 4;
    } else if (!strncmp(start, "false", 5)) {
        v->kind = JSON_BOOL;
        p->cursor += 5;
    } else if (!strncmp(start, "null", 4)) {
        v->kind = JSON_NULL;
        p->cursor += 4;
    } else
        goto fail;
    size_t length = (size_t)(p->cursor - start);
    v->text = malloc(length + 1);
    if (!v->text)
        goto fail;
    memcpy(v->text, start, length);
    v->text[length] = 0;
    return v;
fail:
    ide_json_free(v);
    return NULL;
}
JsonValue *ide_json_parse(const char *text) {
    Parser p = {text, 0};
    JsonValue *v = value(&p, 0);
    if (!v)
        return NULL;
    space(&p);
    if (*p.cursor) {
        ide_json_free(v);
        return NULL;
    }
    return v;
}
const JsonValue *ide_json_get(const JsonValue *object, const char *name) {
    if (!object || object->kind != JSON_OBJECT)
        return NULL;
    for (const JsonValue *v = object->child; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return NULL;
}
const char *ide_json_string(const JsonValue *object, const char *name) {
    const JsonValue *v = ide_json_get(object, name);
    return v && v->kind == JSON_STRING ? v->text : NULL;
}
int ide_json_integer(const JsonValue *v, int64_t *result) {
    if (!v || v->kind != JSON_NUMBER)
        return 0;
    char *end;
    errno = 0;
    long long n = strtoll(v->text, &end, 10);
    if (errno || *end)
        return 0;
    *result = (int64_t)n;
    return 1;
}
void ide_json_quote(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*p < 32)
                fprintf(out, "\\u%04x", *p);
            else
                fputc(*p, out);
        }
    }
    fputc('"', out);
}
