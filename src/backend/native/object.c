#include "object.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int native_buffer_bytes(NativeBuffer *buffer, const void *data, size_t size) {
    if (size > SIZE_MAX - buffer->size) return 0;
    size_t needed = buffer->size + size;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        unsigned char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (size != 0) {
        if (data == NULL) memset(buffer->data + buffer->size, 0, size);
        else memcpy(buffer->data + buffer->size, data, size);
    }
    buffer->size = needed;
    return 1;
}

int native_buffer_uint(NativeBuffer *buffer, uint64_t value, size_t size) {
    unsigned char bytes[8];
    if (size > sizeof(bytes)) return 0;
    for (size_t i = 0; i < size; i++) bytes[i] = (unsigned char) (value >> (i * 8));
    return native_buffer_bytes(buffer, bytes, size);
}

int native_buffer_align(NativeBuffer *buffer, size_t alignment) {
    if (alignment == 0) return 0;
    return native_buffer_bytes(buffer, NULL, (alignment - buffer->size % alignment) % alignment);
}

void native_buffer_patch(NativeBuffer *buffer, size_t offset, uint64_t value, size_t size) {
    if (offset > buffer->size || size > buffer->size - offset || size > 8) return;
    for (size_t i = 0; i < size; i++) buffer->data[offset + i] = (unsigned char) (value >> (i * 8));
}

void native_error(NativeObject *object, const char *message) {
    if (!object->failed) (void) snprintf(object->error, sizeof(object->error), "%s", message);
    object->failed = 1;
}

static int grow(void **items, size_t *capacity, size_t size) {
    size_t next = *capacity == 0 ? 32 : *capacity * 2;
    if (next < *capacity || next > SIZE_MAX / size) return 0;
    void *result = realloc(*items, next * size);
    if (result == NULL) return 0;
    *items = result;
    *capacity = next;
    return 1;
}

size_t native_symbol(NativeObject *object, const char *name) {
    if (name == NULL || name[0] == '\0') {
        native_error(object, "Empty native symbol");
        return SIZE_MAX;
    }
    for (size_t i = 0; i < object->symbol_count; i++)
        if (strcmp(object->symbols[i].name, name) == 0) return i;
    if (object->symbol_count == object->symbol_capacity &&
        !grow((void **) &object->symbols, &object->symbol_capacity, sizeof(*object->symbols))) {
        native_error(object, "Out of memory for native symbols");
        return SIZE_MAX;
    }
    size_t size = strlen(name) + 1;
    char *copy = malloc(size);
    if (copy == NULL) {
        native_error(object, "Out of memory for native symbol name");
        return SIZE_MAX;
    }
    memcpy(copy, name, size);
    size_t index = object->symbol_count++;
    object->symbols[index] = (NativeSymbol)
    {
        .name = copy, .section = -1, .global = name[0] != '.'
    };
    return index;
}

int native_define(NativeObject *object, const char *name, int global, int function) {
    size_t index = native_symbol(object, name);
    if (index == SIZE_MAX) return 0;
    NativeSymbol *symbol = &object->symbols[index];
    if (symbol->defined) {
        native_error(object, "Duplicate native symbol definition");
        return 0;
    }
    symbol->defined = 1;
    symbol->global = global;
    symbol->function = function;
    symbol->section = (int) object->section;
    symbol->offset = object->sections[object->section].size;
    return 1;
}

int native_bytes(NativeObject *object, const void *bytes, size_t size) {
    if (!native_buffer_bytes(&object->sections[object->section], bytes, size)) {
        native_error(object, "Out of memory for native section");
        return 0;
    }
    return !object->failed;
}

int native_uint(NativeObject *object, uint64_t value, size_t size) {
    if (!native_buffer_uint(&object->sections[object->section], value, size)) {
        native_error(object, "Out of memory for native section");
        return 0;
    }
    return !object->failed;
}

int native_reference(NativeObject *object, const char *name, NativeRelocKind kind,
                     size_t offset, int64_t addend) {
    size_t symbol = native_symbol(object, name);
    if (symbol == SIZE_MAX) return 0;
    if (object->relocation_count == object->relocation_capacity &&
        !grow((void **) &object->relocations, &object->relocation_capacity, sizeof(*object->relocations))) {
        native_error(object, "Out of memory for native relocations");
        return 0;
    }
    object->relocations[object->relocation_count++] =
            (NativeRelocation)
    {
        object->section, offset, symbol, kind, addend
    };
    return 1;
}

void native_object_free(NativeObject *object) {
    for (size_t i = 0; i < NATIVE_SECTION_COUNT; i++) free(object->sections[i].data);
    for (size_t i = 0; i < object->symbol_count; i++) free(object->symbols[i].name);
    free(object->symbols);
    free(object->relocations);
    *object = (NativeObject)
    {
        0
    };
}

/* Resolve references within a section; keep cross-section/external relocations. */
static int resolve_local(NativeObject *object) {
    size_t kept = 0;
    for (size_t i = 0; i < object->relocation_count; i++) {
        NativeRelocation relocation = object->relocations[i];
        NativeSymbol *symbol = &object->symbols[relocation.symbol];
        if (!symbol->defined && symbol->name[0] == '.') {
            native_error(object, "Undefined native local label");
            return 0;
        }
        if (relocation.kind != NATIVE_ADDR64 && symbol->defined && symbol->section == (int) relocation.section) {
            if (symbol->offset > INT64_MAX || relocation.offset > INT64_MAX) return 0;
            int64_t value = (int64_t) symbol->offset - (int64_t) relocation.offset + relocation.addend;
            if (value < INT32_MIN || value > INT32_MAX) {
                native_error(object, "Native branch exceeds signed 32-bit displacement");
                return 0;
            }
            native_buffer_patch(&object->sections[relocation.section], relocation.offset, (uint64_t) value, 4);
        } else object->relocations[kept++] = relocation;
    }
    object->relocation_count = kept;
    return !object->failed;
}

#define PUT(b, v, n) do { if (!native_buffer_uint((b), (uint64_t) (v), (n))) goto failure; } while (0)
#define BYTES(b, p, n) do { if (!native_buffer_bytes((b), (p), (n))) goto failure; } while (0)
#define ALIGN(b, n) do { if (!native_buffer_align((b), (n))) goto failure; } while (0)

static int write_elf(NativeObject *object, NativeBuffer *output) {
    enum { SECTIONS = 11, SYMTAB = 7, STRTAB = 8, SHSTRTAB = 9 };
    NativeBuffer names = {0}, strings = {0}, symbols = {0}, relocations[3] = {{0}};
    size_t *map = NULL;
    size_t offsets[SECTIONS] = {0}, sizes[SECTIONS] = {0}, name_offsets[SECTIONS] = {0};
    static const char *section_names[] = {
        "", ".text", ".rodata", ".data", ".rela.text",
        ".rela.rodata", ".rela.data", ".symtab", ".strtab", ".shstrtab", ".note.GNU-stack"
    };
    if (object->symbol_count > (UINT32_MAX - 1) || object->symbol_count > SIZE_MAX / sizeof(*map)) goto failure;
    map = malloc(object->symbol_count * sizeof(*map));
    if (map == NULL && object->symbol_count != 0) goto failure;
    for (size_t s = 0; s < SECTIONS; s++) {
        name_offsets[s] = names.size;
        BYTES(&names, section_names[s], strlen(section_names[s]) + 1);
    }
    PUT(&strings, 0, 1);
    BYTES(&symbols, NULL, 24);
    size_t index = 1, first_global = 1;
    for (int global = 0; global <= 1; global++) {
        if (global) first_global = index;
        for (size_t i = 0; i < object->symbol_count; i++) {
            const NativeSymbol *symbol = &object->symbols[i];
            if (symbol->global != global) continue;
            if (strings.size > UINT32_MAX) goto failure;
            map[i] = index++;
            PUT(&symbols, strings.size, 4);
            PUT(&symbols, (global << 4) | (symbol->function ? 2 : 0), 1);
            PUT(&symbols, 0, 1);
            PUT(&symbols, symbol->defined ? symbol->section + 1 : 0, 2);
            PUT(&symbols, symbol->defined ? symbol->offset : 0, 8);
            PUT(&symbols, symbol->size, 8);
            BYTES(&strings, symbol->name, strlen(symbol->name) + 1);
        }
    }
    for (size_t i = 0; i < object->relocation_count; i++) {
        const NativeRelocation *relocation = &object->relocations[i];
        NativeBuffer *buffer = &relocations[relocation->section];
        PUT(buffer, relocation->offset, 8);
        PUT(buffer, ((uint64_t) map[relocation->symbol] << 32) |
            (relocation->kind == NATIVE_ADDR64 ? 1U : relocation->kind == NATIVE_CALL32 ? 4U : 2U), 8);
        PUT(buffer, relocation->addend, 8);
    }
    BYTES(output, NULL, 64);
    for (size_t s = 1; s < SECTIONS; s++) {
        NativeBuffer *buffer = s <= 3
                                   ? &object->sections[s - 1]
                                   : s <= 6
                                         ? &relocations[s - 4]
                                         : s == SYMTAB
                                               ? &symbols
                                               : s == STRTAB
                                                     ? &strings
                                                     : s == SHSTRTAB
                                                           ? &names
                                                           : NULL;
        ALIGN(output, s <= 3 ? 16 : 8);
        offsets[s] = output->size;
        sizes[s] = buffer == NULL ? 0 : buffer->size;
        if (buffer != NULL)
            BYTES(output, buffer->data, buffer->size);
    }
    ALIGN(output, 8);
    size_t headers = output->size;
    BYTES(output, NULL, 64);
    for (size_t s = 1; s < SECTIONS; s++) {
        PUT(output, name_offsets[s], 4);
        PUT(output, s >= 4 && s <= 6 ? 4 : s == SYMTAB ? 2 : s == STRTAB || s == SHSTRTAB ? 3 : 1, 4);
        PUT(output, s == 1 ? 6 : s == 2 ? 2 : s == 3 ? 3 : 0, 8);
        PUT(output, 0, 8);
        PUT(output, offsets[s], 8);
        PUT(output, sizes[s], 8);
        PUT(output, s >= 4 && s <= 6 ? SYMTAB : s == SYMTAB ? STRTAB : 0, 4);
        PUT(output, s >= 4 && s <= 6 ? s - 3 : s == SYMTAB ? first_global : 0, 4);
        PUT(output, s <= 3 ? 16 : s <= 7 ? 8 : 1, 8);
        PUT(output, s >= 4 && s <= 7 ? 24 : 0, 8);
    }
    memcpy(output->data, "\177ELF\2\1\1", 7);
    native_buffer_patch(output, 16, 1, 2);
    native_buffer_patch(output, 18, 62, 2);
    native_buffer_patch(output, 20, 1, 4);
    native_buffer_patch(output, 40, headers, 8);
    native_buffer_patch(output, 52, 64, 2);
    native_buffer_patch(output, 58, 64, 2);
    native_buffer_patch(output, 60, SECTIONS, 2);
    native_buffer_patch(output, 62, SHSTRTAB, 2);
    free(map);
    free(names.data);
    free(strings.data);
    free(symbols.data);
    for (size_t s = 0; s < 3; s++) free(relocations[s].data);
    return 1;
failure:
    free(map);
    free(names.data);
    free(strings.data);
    free(symbols.data);
    for (size_t s = 0; s < 3; s++) free(relocations[s].data);
    native_error(object, "Could not serialize ELF object (allocation or format limit)");
    return 0;
}

static int write_coff(NativeObject *object, NativeBuffer *output) {
    NativeBuffer strings = {0};
    size_t raw[3] = {0}, reloc[3] = {0}, counts[3] = {0};
    static const char *names[] = {".text", ".rdata", ".data"};
    if (object->symbol_count > UINT32_MAX) goto failure;
    BYTES(output, NULL, 20 + 3 * 40);
    for (size_t i = 0; i < object->relocation_count; i++) counts[object->relocations[i].section]++;
    for (size_t s = 0; s < 3; s++) {
        ALIGN(output, 16);
        raw[s] = output->size;
        NativeBuffer *section = &object->sections[s];
        BYTES(output, section->data, section->size);
        reloc[s] = output->size;
        if (counts[s] >= 65535) {
            PUT(output, counts[s] + 1, 4);
            PUT(output, 0, 4);
            PUT(output, 0, 2);
        }
        for (size_t i = 0; i < object->relocation_count; i++) {
            const NativeRelocation *r = &object->relocations[i];
            if ((size_t) r->section != s) continue;
            native_buffer_patch(output, raw[s] + r->offset,
                                (uint64_t)(r->addend + (r->kind == NATIVE_ADDR64 ? 0 : 4)),
                                r->kind == NATIVE_ADDR64 ? 8 : 4);
            PUT(output, r->offset, 4);
            PUT(output, r->symbol, 4);
            PUT(output, r->kind == NATIVE_ADDR64 ? 1 : 4, 2);
        }
    }
    size_t symtab = output->size;
    PUT(&strings, 4, 4);
    for (size_t i = 0; i < object->symbol_count; i++) {
        NativeSymbol *symbol = &object->symbols[i];
        size_t length = strlen(symbol->name);
        if (length <= 8) {
            BYTES(output, symbol->name, length);
            BYTES(output, NULL, 8 - length);
        } else {
            PUT(output, 0, 4);
            PUT(output, strings.size, 4);
            BYTES(&strings, symbol->name, length + 1);
        }
        PUT(output, symbol->defined ? symbol->offset : 0, 4);
        PUT(output, symbol->defined ? symbol->section + 1 : 0, 2);
        PUT(output, symbol->function ? 32 : 0, 2);
        PUT(output, symbol->global ? 2 : 3, 1);
        PUT(output, 0, 1);
    }
    if (strings.size > UINT32_MAX || output->size > UINT32_MAX - strings.size) goto failure;
    native_buffer_patch(&strings, 0, strings.size, 4);
    BYTES(output, strings.data, strings.size);
    native_buffer_patch(output, 0, 0x8664, 2);
    native_buffer_patch(output, 2, 3, 2);
    native_buffer_patch(output, 8, symtab, 4);
    native_buffer_patch(output, 12, object->symbol_count, 4);
    for (size_t s = 0; s < 3; s++) {
        size_t h = 20 + s * 40;
        memcpy(output->data + h, names[s], strlen(names[s]));
        native_buffer_patch(output, h + 16, object->sections[s].size, 4);
        native_buffer_patch(output, h + 20, raw[s], 4);
        native_buffer_patch(output, h + 24, counts[s] == 0 ? 0 : reloc[s], 4);
        native_buffer_patch(output, h + 32, counts[s] >= 65535 ? 65535 : counts[s], 2);
        native_buffer_patch(output, h + 36, (s == 0 ? 0x60500020U : s == 1 ? 0x40500040U : 0xc0500040U) |
                                            (counts[s] >= 65535 ? 0x01000000U : 0), 4);
    }
    free(strings.data);
    return 1;
failure:
    free(strings.data);
    native_error(object, "Could not serialize COFF object (allocation or format limit)");
    return 0;
}

int native_write_object(NativeObject *object, TargetFormat target, NativeBuffer *output) {
    if (!native_validate(object) || !resolve_local(object)) return 0;
    return target == TARGET_ELF ? write_elf(object, output) : target == TARGET_COFF ? write_coff(object, output) : 0;
}

int native_validate(NativeObject *object) {
    if (!object || object->failed) return 0;
    for (size_t n = 0; n < object->symbol_count; ++n) {
        const NativeSymbol *s = &object->symbols[n];
        if (!s->name || (s->defined && (s->section < 0 || s->section >= NATIVE_SECTION_COUNT ||
                                        s->offset > object->sections[s->section].size || s->size > object->sections[s->
                                            section].size - s->offset))) {
            native_error(object, "Invalid native symbol bounds");
            return 0;
        }
    }
    for (size_t n = 0; n < object->relocation_count; ++n) {
        const NativeRelocation *r = &object->relocations[n];
        size_t width = r->kind == NATIVE_ADDR64 ? 8 : 4;
        if (r->section < 0 || r->section >= NATIVE_SECTION_COUNT || r->symbol >= object->symbol_count ||
            r->kind < 0 || r->kind > NATIVE_ADDR64 || r->offset > object->sections[r->section].size ||
            width > object->sections[r->section].size - r->offset ||
            (r->kind != NATIVE_ADDR64 && (r->addend < INT32_MIN || r->addend > INT32_MAX))) {
            native_error(object, "Invalid native relocation bounds");
            return 0;
        }
    }
    return 1;
}
