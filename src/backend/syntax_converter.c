#include "syntax_converter.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} String;

typedef struct {
    const char *att;
    const char *intel;
    const char *source_qualifier;
} Mnemonic;

static const Mnemonic mnemonics[] = {
    {"movq", "mov ", NULL}, {"movl", "mov ", NULL}, {"movw", "mov ", NULL}, {"movb", "mov ", NULL},
    {"movzbl", "movzx ", "BYTE PTR "}, {"movzwl", "movzx ", "WORD PTR "},
    {"movzlq", "movzx ", "DWORD PTR "}, {"movsbl", "movsx ", "BYTE PTR "},
    {"movswl", "movsx ", "WORD PTR "}, {"movslq", "movsx ", "DWORD PTR "},
    {"pushq", "push ", NULL}, {"popq", "pop ", NULL},
    {"addq", "add ", NULL}, {"addl", "add ", NULL}, {"subq", "sub ", NULL}, {"subl", "sub ", NULL},
    {"leaq", "lea ", NULL}, {"leal", "lea ", NULL}, {"xorl", "xor ", NULL}, {"xorq", "xor ", NULL},
    {"cmpl", "cmp ", NULL}, {"cmpq", "cmp ", NULL},
    {"testb", "test ", NULL}, {"testl", "test ", NULL}, {"testq", "test ", NULL},
    {"imull", "imul ", NULL}, {"imulq", "imul ", NULL}, {"idivl", "idiv ", NULL},
    {"idivq", "idiv ", NULL}, {"negl", "neg ", NULL}, {"negq", "neg ", NULL},
    {"incq", "inc ", NULL}, {"decq", "dec ", NULL}, {"andl", "and ", NULL}, {"andq", "and ", NULL},
    {"orl", "or ", NULL}, {"orq", "or ", NULL},
    {"sete", NULL, NULL}, {"setne", NULL, NULL}, {"setl", NULL, NULL}, {"setle", NULL, NULL},
    {"setg", NULL, NULL}, {"setge", NULL, NULL}
};

static int reserve(String *string, size_t needed) {
    if (needed == SIZE_MAX) return 0;
    needed++;
    if (needed <= string->capacity) return 1;
    size_t capacity = string->capacity == 0 ? 128U : string->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    char *grown = realloc(string->data, capacity);
    if (grown == NULL) return 0;
    string->data = grown;
    string->capacity = capacity;
    return 1;
}

static int append_n(String *string, const char *text, size_t length) {
    if (length > SIZE_MAX - string->length || !reserve(string, string->length + length)) return 0;
    if (length != 0) memcpy(string->data + string->length, text, length);
    string->length += length;
    string->data[string->length] = '\0';
    return 1;
}

static int append(String *string, const char *text) {
    return append_n(string, text, strlen(text));
}

static int assign(String *string, const char *text) {
    string->length = 0;
    return append(string, text);
}

static int replace(String *string, size_t begin, size_t end, const char *text) {
    if (begin > end || end > string->length) return 0;
    size_t replacement = strlen(text);
    size_t removed = end - begin;
    if (replacement > removed && replacement - removed > SIZE_MAX - string->length) return 0;
    size_t length = string->length - removed + replacement;
    if (!reserve(string, length)) return 0;
    memmove(string->data + begin + replacement, string->data + end, string->length - end + 1U);
    if (replacement != 0) memcpy(string->data + begin, text, replacement);
    string->length = length;
    return 1;
}

static int append_slice(String *destination, const String *source, size_t begin, size_t end) {
    return begin <= end && end <= source->length && append_n(destination, source->data + begin, end - begin);
}

static const Mnemonic *find_mnemonic(const char *text, size_t length) {
    for (size_t i = 0; i < sizeof(mnemonics) / sizeof(mnemonics[0]); i++)
        if (strlen(mnemonics[i].att) == length && memcmp(text, mnemonics[i].att, length) == 0)
            return &mnemonics[i];
    return NULL;
}

static int strip_prefixes(const char *input, String *result) {
    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char next = (unsigned char) input[i + 1U];
        if (input[i] == '%' && (isalpha(next) || next == 'r' || next == 'e')) continue;
        if (input[i] == '$' && (isalnum(next) || next == '-' || next == '.' || next == '_')) continue;
        if (!append_n(result, input + i, 1U)) return 0;
    }
    return 1;
}

static int append_address(String *replacement, const char *content, size_t content_length,
                          const char *offset, size_t offset_length) {
    const char *comma1 = memchr(content, ',', content_length);
    if (!append(replacement, "[")) return 0;
    if (comma1 == NULL) {
        if (!append_n(replacement, content, content_length)) return 0;
    } else {
        size_t base_length = (size_t) (comma1 - content);
        const char *after_comma = comma1 + 1;
        size_t remaining = content_length - base_length - 1U;
        const char *comma2 = memchr(after_comma, ',', remaining);
        size_t index_length = comma2 == NULL ? remaining : (size_t) (comma2 - after_comma);
        if (!append_n(replacement, content, base_length) || !append(replacement, "+") ||
            !append_n(replacement, after_comma, index_length))
            return 0;
        if (comma2 != NULL && (size_t) (content + content_length - comma2 - 1) != 1U) {
            if (!append(replacement, "*") ||
                !append_n(replacement, comma2 + 1, (size_t) (content + content_length - comma2 - 1)))
                return 0;
        } else if (comma2 != NULL && comma2[1] != '1') {
            if (!append(replacement, "*") || !append_n(replacement, comma2 + 1, 1U)) return 0;
        }
    }
    if (offset_length != 0) {
        if (offset[0] != '-' && !append(replacement, "+")) return 0;
        if (!append_n(replacement, offset, offset_length)) return 0;
    }
    return append(replacement, "]");
}

static int convert_addresses(String *result) {
    size_t search = 0;
    while (search < result->length) {
        char *open_pointer = strchr(result->data + search, '(');
        if (open_pointer == NULL) break;
        size_t open = (size_t) (open_pointer - result->data);
        char *close_pointer = strchr(open_pointer + 1, ')');
        if (close_pointer == NULL) {
            search = open + 1U;
            continue;
        }
        size_t close = (size_t) (close_pointer - result->data);
        size_t begin = open;
        String replacement = {0};
        int ok;
        if (close - open == 4U && memcmp(result->data + open + 1U, "rip", 3U) == 0) {
            while (begin > 0 && (isalnum((unsigned char) result->data[begin - 1U]) ||
                                  result->data[begin - 1U] == '_' || result->data[begin - 1U] == '.' ||
                                  result->data[begin - 1U] == '-'))
                begin--;
            ok = append(&replacement, "[rip + ") && append_n(&replacement, result->data + begin, open - begin) &&
                 append(&replacement, "]");
        } else {
            while (begin > 0 && (isdigit((unsigned char) result->data[begin - 1U]) ||
                                  result->data[begin - 1U] == '-'))
                begin--;
            ok = append_address(&replacement, result->data + open + 1U, close - open - 1U,
                                result->data + begin, open - begin);
        }
        if (!ok || !replace(result, begin, close + 1U, replacement.data == NULL ? "" : replacement.data)) {
            free(replacement.data);
            return 0;
        }
        search = begin + replacement.length;
        free(replacement.data);
    }
    return 1;
}

static int reverse_operands(String *result, size_t instruction, const char *qualifier) {
    size_t operands = instruction;
    while (operands < result->length && !isspace((unsigned char) result->data[operands])) operands++;
    while (operands < result->length && (result->data[operands] == ' ' || result->data[operands] == '\t')) operands++;
    char *comma_pointer = operands < result->length ? strchr(result->data + operands, ',') : NULL;
    if (comma_pointer == NULL) return 1;
    size_t comma = (size_t) (comma_pointer - result->data);
    size_t first_end = comma;
    while (first_end > operands && (result->data[first_end - 1U] == ' ' || result->data[first_end - 1U] == '\t'))
        first_end--;
    size_t second = comma + 1U;
    while (second < result->length && (result->data[second] == ' ' || result->data[second] == '\t')) second++;
    size_t second_end = second;
    while (second_end < result->length && result->data[second_end] != '\n' && result->data[second_end] != '#' &&
           result->data[second_end] != ';')
        second_end++;
    while (second_end > second && isspace((unsigned char) result->data[second_end - 1U]) &&
           result->data[second_end - 1U] != '\n')
        second_end--;

    String reversed = {0};
    int memory_source = qualifier != NULL && operands < result->length && result->data[operands] == '[';
    int ok = append_slice(&reversed, result, 0, operands) && append_slice(&reversed, result, second, second_end) &&
             append(&reversed, ", ") && (!memory_source || append(&reversed, qualifier)) &&
             append_slice(&reversed, result, operands, first_end) &&
             append_slice(&reversed, result, second_end, result->length);
    if (ok) ok = assign(result, reversed.data == NULL ? "" : reversed.data);
    free(reversed.data);
    return ok;
}

int convert_att_to_intel(const char *input, char *output, size_t output_size) {
    if (input == NULL || output == NULL || output_size == 0) return -1;
    output[0] = '\0';
    size_t instruction = 0;
    while (input[instruction] == ' ' || input[instruction] == '\t') instruction++;
    size_t instruction_end = instruction;
    while (input[instruction_end] != '\0' && !isspace((unsigned char) input[instruction_end])) instruction_end++;
    const Mnemonic *mnemonic = find_mnemonic(input + instruction, instruction_end - instruction);
    if (mnemonic == NULL && strchr(input + instruction, '%') == NULL) {
        size_t length = strlen(input);
        if (length >= output_size) return -1;
        memcpy(output, input, length + 1U);
        return 0;
    }

    String result = {0};
    if (!strip_prefixes(input, &result)) goto fail;
    if (mnemonic != NULL && mnemonic->intel != NULL &&
        !replace(&result, instruction, instruction_end, mnemonic->intel))
        goto fail;
    if (!convert_addresses(&result) || !reverse_operands(&result, instruction,
                                                         mnemonic == NULL ? NULL : mnemonic->source_qualifier))
        goto fail;
    if (result.length >= output_size) goto fail;
    memcpy(output, result.data, result.length + 1U);
    free(result.data);
    return 1;

fail:
    free(result.data);
    return -1;
}
