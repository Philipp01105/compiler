#include "syntax_converter.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define CONVERTER_WORK_SIZE 16384
#define CONVERTER_INPUT_SIZE 4096

static int append_text(char *destination, size_t capacity, size_t *length, const char *text) {
    size_t text_length = strlen(text);
    if (text_length >= capacity || *length > capacity - text_length - 1) return 0;
    memcpy(destination + *length, text, text_length);
    *length += text_length;
    destination[*length] = '\0';
    return 1;
}

/*
 * Simple pattern-based converter using string substitution
 */
int convert_att_to_intel(const char *input, char *output, size_t output_size) {
    if (input == NULL || output == NULL || output_size == 0) return -1;
    if (strlen(input) >= output_size || strlen(input) >= CONVERTER_INPUT_SIZE) return -1;
    /* Check if this looks like an AT&T instruction */
    const char *p = input;

    /* Skip whitespace */
    while (*p == ' ' || *p == '\t') p++;

    /* Check for AT&T instruction patterns */
    int is_att = 0;
    if (strstr(p, "movq") || strstr(p, "movl") || strstr(p, "movw") || strstr(p, "movb") ||
        strstr(p, "movzbl") || strstr(p, "movzwl") || strstr(p, "movzlq") ||
        strstr(p, "movsbl") || strstr(p, "movswl") || strstr(p, "movslq") ||
        strstr(p, "pushq") || strstr(p, "popq") ||
        strstr(p, "addq") || strstr(p, "addl") ||
        strstr(p, "subq") || strstr(p, "subl") ||
        strstr(p, "leaq") || strstr(p, "leal") ||
        strstr(p, "xorl") || strstr(p, "xorq") ||
        strstr(p, "cmpl") || strstr(p, "cmpq") ||
        strstr(p, "testb") || strstr(p, "testl") || strstr(p, "testq") ||
        strstr(p, "imull") || strstr(p, "imulq") ||
        strstr(p, "idivl") || strstr(p, "idivq") ||
        strstr(p, "negl") || strstr(p, "negq") ||
        strstr(p, "incq") || strstr(p, "decq") ||
        strstr(p, "andl") || strstr(p, "andq") ||
        strstr(p, "orl") || strstr(p, "orq") ||
        strstr(p, "sete") || strstr(p, "setne") || strstr(p, "setl") ||
        strstr(p, "setle") || strstr(p, "setg") || strstr(p, "setge")) {
        is_att = 1;
    }
    if (strchr(p, '%') != NULL) is_att = 1;

    if (!is_att) {
        /* Not AT&T syntax, copy as-is */
        if (strlen(input) >= output_size) return -1;
        strncpy(output, input, output_size - 1);
        output[output_size - 1] = '\0';
        return 0;
    }

    /* Simple conversion: Handle common patterns */
    char temp[CONVERTER_WORK_SIZE];
    if (strlen(input) >= sizeof(temp)) return -1;
    strncpy(temp, input, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    /* Remove % from registers */
    char *src = temp;
    char *dst = output;
    size_t remaining = output_size;

    while (*src && remaining > 1) {
        if (*src == '%' && (isalpha(src[1]) || src[1] == 'r' || src[1] == 'e')) {
            /* Skip % before register names */
            src++;
        } else if (*src == '$' && (isalnum((unsigned char) src[1]) || src[1] == '-' ||
                                    src[1] == '.' || src[1] == '_')) {
            /* Skip $ before immediates */
            src++;
        } else {
            *dst++ = *src++;
            remaining--;
        }
    }
    *dst = '\0';

    /* Now handle instruction suffixes and operand reversal */
    /* This is a simplified version - just remove suffixes for now */
    char final[CONVERTER_WORK_SIZE];
    strncpy(final, output, sizeof(final) - 1);
    final[sizeof(final) - 1] = '\0';

    /* Remove instruction suffixes (q, l, w, b) */
    /* Track the source operand size for movsx/movzx instructions */
    const char *src_size_qualifier = NULL;
    char *insn_start = final;
    while (*insn_start == ' ' || *insn_start == '\t') insn_start++;

    if (strncmp(insn_start, "movq ", 5) == 0 && strstr(insn_start, "xmm") == NULL) {
        memcpy(insn_start, "mov ", 4);
    }
    else if (strncmp(insn_start, "movl ", 5) == 0) memcpy(insn_start, "mov ", 4);
    else if (strncmp(insn_start, "movw ", 5) == 0) memcpy(insn_start, "mov ", 4);
    else if (strncmp(insn_start, "movb ", 5) == 0) memcpy(insn_start, "mov ", 4);
    else if (strncmp(insn_start, "movzbl ", 7) == 0) { memcpy(insn_start, "movzx ", 6); src_size_qualifier = "BYTE PTR "; }
    else if (strncmp(insn_start, "movzwl ", 7) == 0) { memcpy(insn_start, "movzx ", 6); src_size_qualifier = "WORD PTR "; }
    else if (strncmp(insn_start, "movzlq ", 7) == 0) { memcpy(insn_start, "movzx ", 6); src_size_qualifier = "DWORD PTR "; }
    else if (strncmp(insn_start, "movsbl ", 7) == 0) { memcpy(insn_start, "movsx ", 6); src_size_qualifier = "BYTE PTR "; }
    else if (strncmp(insn_start, "movswl ", 7) == 0) { memcpy(insn_start, "movsx ", 6); src_size_qualifier = "WORD PTR "; }
    else if (strncmp(insn_start, "movslq ", 7) == 0) { memcpy(insn_start, "movsx ", 6); src_size_qualifier = "DWORD PTR "; }
    else if (strncmp(insn_start, "movss ", 6) == 0) memcpy(insn_start, "movss ", 6); /* Keep movss */
    else if (strncmp(insn_start, "pushq ", 6) == 0) memcpy(insn_start, "push ", 5);
    else if (strncmp(insn_start, "popq ", 5) == 0) memcpy(insn_start, "pop ", 4);
    else if (strncmp(insn_start, "addq ", 5) == 0) memcpy(insn_start, "add ", 4);
    else if (strncmp(insn_start, "addl ", 5) == 0) memcpy(insn_start, "add ", 4);
    else if (strncmp(insn_start, "subq ", 5) == 0) memcpy(insn_start, "sub ", 4);
    else if (strncmp(insn_start, "subl ", 5) == 0) memcpy(insn_start, "sub ", 4);
    else if (strncmp(insn_start, "leaq ", 5) == 0) memcpy(insn_start, "lea ", 4);
    else if (strncmp(insn_start, "leal ", 5) == 0) memcpy(insn_start, "lea ", 4);
    else if (strncmp(insn_start, "xorl ", 5) == 0) memcpy(insn_start, "xor ", 4);
    else if (strncmp(insn_start, "xorq ", 5) == 0) memcpy(insn_start, "xor ", 4);
    else if (strncmp(insn_start, "cmpl ", 5) == 0) memcpy(insn_start, "cmp ", 4);
    else if (strncmp(insn_start, "cmpq ", 5) == 0) memcpy(insn_start, "cmp ", 4);
    else if (strncmp(insn_start, "testb ", 6) == 0) memcpy(insn_start, "test ", 5);
    else if (strncmp(insn_start, "testl ", 6) == 0) memcpy(insn_start, "test ", 5);
    else if (strncmp(insn_start, "testq ", 6) == 0) memcpy(insn_start, "test ", 5);
    else if (strncmp(insn_start, "imull ", 6) == 0) memcpy(insn_start, "imul ", 5);
    else if (strncmp(insn_start, "imulq ", 6) == 0) memcpy(insn_start, "imul ", 5);
    else if (strncmp(insn_start, "idivl ", 6) == 0) memcpy(insn_start, "idiv ", 5);
    else if (strncmp(insn_start, "idivq ", 6) == 0) memcpy(insn_start, "idiv ", 5);
    else if (strncmp(insn_start, "negl ", 5) == 0) memcpy(insn_start, "neg ", 4);
    else if (strncmp(insn_start, "negq ", 5) == 0) memcpy(insn_start, "neg ", 4);
    else if (strncmp(insn_start, "incq ", 5) == 0) memcpy(insn_start, "inc ", 4);
    else if (strncmp(insn_start, "decq ", 5) == 0) memcpy(insn_start, "dec ", 4);
    else if (strncmp(insn_start, "andl ", 5) == 0) memcpy(insn_start, "and ", 4);
    else if (strncmp(insn_start, "andq ", 5) == 0) memcpy(insn_start, "and ", 4);
    else if (strncmp(insn_start, "orl ", 4) == 0) memcpy(insn_start, "or ", 3);
    else if (strncmp(insn_start, "orq ", 4) == 0) memcpy(insn_start, "or ", 3);

    /* Handle memory addressing: (reg) -> [reg],  offset(reg) -> [reg+offset] */
    /* Handle RIP-relative: label(rip) -> [rel label] */
    char *result = final;
    char *paren = result;
    while ((paren = strchr(paren, '(')) != NULL) {
        /* Check if this is (rip) */
        if (strncmp(paren, "(rip)", 5) == 0) {
            /* Find the label before ( */
            char *label_end = paren;
            char *label_start = label_end;
            while (label_start > result && (isalnum((unsigned char) label_start[-1]) || label_start[-1] == '_' ||
                                             label_start[-1] == '.' || label_start[-1] == '-')) {
                --label_start;
            }
            /* Extract label */
            size_t label_len = (size_t) (label_end - label_start);
            char label[CONVERTER_WORK_SIZE];
            strncpy(label, label_start, label_len);
            label[label_len] = '\0';

            /* Replace with [rip + label] for GAS Intel syntax */
            char replacement[CONVERTER_WORK_SIZE];
            size_t replacement_length = 0;
            replacement[0] = '\0';
            if (!append_text(replacement, sizeof(replacement), &replacement_length, "[rip + ") ||
                !append_text(replacement, sizeof(replacement), &replacement_length, label) ||
                !append_text(replacement, sizeof(replacement), &replacement_length, "]")) return -1;

            /* Build new string */
            size_t prefix_len = (size_t) (label_start - result);
            char new_result[CONVERTER_WORK_SIZE];
            strncpy(new_result, result, prefix_len);
            new_result[prefix_len] = '\0';
            strcat(new_result, replacement);
            strcat(new_result, paren + 5); /* Skip (rip) */

            strcpy(final, new_result);
            paren = final + prefix_len + strlen(replacement);
        } else {
            /* Regular memory operand: offset(reg) -> [reg+offset] or (base,index,scale) -> [base+index*scale] */
            char *close_paren = strchr(paren + 1, ')');
            if (close_paren) {
                /* Extract content inside parentheses */
                size_t content_len = (size_t) (close_paren - paren - 1);
                char content[CONVERTER_WORK_SIZE];
                strncpy(content, paren + 1, content_len);
                content[content_len] = '\0';

                /* Check if this is indexed addressing (has commas) */
                char *comma1 = strchr(content, ',');
                if (comma1) {
                    /* Indexed addressing: (base,index,scale) */
                    char base[CONVERTER_WORK_SIZE], index[CONVERTER_WORK_SIZE], scale[CONVERTER_WORK_SIZE] = "1";

                    /* Extract base */
                    size_t base_len = (size_t) (comma1 - content);
                    strncpy(base, content, base_len);
                    base[base_len] = '\0';

                    /* Extract index */
                    char *comma2 = strchr(comma1 + 1, ',');
                    if (comma2) {
                        /* Has scale */
                        size_t index_len = (size_t) (comma2 - comma1 - 1);
                        strncpy(index, comma1 + 1, index_len);
                        index[index_len] = '\0';

                        /* Extract scale */
                        strcpy(scale, comma2 + 1);
                    } else {
                        /* No scale, just base and index */
                        strcpy(index, comma1 + 1);
                    }

                    /* Find offset before parenthesis */
                    char *offset_end = paren;
                    char *offset_start = offset_end;
                    while (offset_start > result && (isdigit((unsigned char) offset_start[-1]) || offset_start[-1] == '-')) {
                        --offset_start;
                    }
                    char offset[CONVERTER_WORK_SIZE] = "";
                    if (offset_start < offset_end) {
                        size_t offset_len = (size_t) (offset_end - offset_start);
                        strncpy(offset, offset_start, offset_len);
                        offset[offset_len] = '\0';
                    }

                    /* Build Intel syntax: [base+index*scale+offset] or [base+index*scale] */
                    char replacement[CONVERTER_WORK_SIZE];
                    strcpy(replacement, "[");
                    strcat(replacement, base);
                    strcat(replacement, "+");
                    strcat(replacement, index);
                    if (strcmp(scale, "1") != 0) {
                        strcat(replacement, "*");
                        strcat(replacement, scale);
                    }
                    if (offset[0]) {
                        int offset_val = atoi(offset);
                        if (offset_val >= 0) {
                            strcat(replacement, "+");
                        }
                        strcat(replacement, offset);
                    }
                    strcat(replacement, "]");

                    /* Build new string */
                    size_t prefix_len = (size_t) (offset_start - result);
                    char new_result[CONVERTER_WORK_SIZE];
                    strncpy(new_result, result, prefix_len);
                    new_result[prefix_len] = '\0';
                    strcat(new_result, replacement);
                    strcat(new_result, close_paren + 1);

                    strcpy(final, new_result);
                    paren = final + prefix_len + strlen(replacement);
                    continue;
                }

                /* Simple addressing: (reg) or offset(reg) */
                char reg[CONVERTER_WORK_SIZE];
                strcpy(reg, content);

                /* Find offset before parenthesis */
                char *offset_end = paren;
                char *offset_start = offset_end;
                while (offset_start > result && (isdigit((unsigned char) offset_start[-1]) || offset_start[-1] == '-')) {
                    --offset_start;
                }
                char offset[CONVERTER_WORK_SIZE] = "";
                if (offset_start < offset_end) {
                    size_t offset_len = (size_t) (offset_end - offset_start);
                    strncpy(offset, offset_start, offset_len);
                    offset[offset_len] = '\0';
                }

                /* Build [reg+offset] or [reg-offset] or [reg] */
                char replacement[CONVERTER_WORK_SIZE];
                size_t replacement_length = 0;
                replacement[0] = '\0';
                if (offset[0]) {
                    int offset_val = atoi(offset);
                    if (!append_text(replacement, sizeof(replacement), &replacement_length, "[") ||
                        !append_text(replacement, sizeof(replacement), &replacement_length, reg) ||
                        (offset_val >= 0 && !append_text(replacement, sizeof(replacement), &replacement_length, "+")) ||
                        !append_text(replacement, sizeof(replacement), &replacement_length, offset) ||
                        !append_text(replacement, sizeof(replacement), &replacement_length, "]")) return -1;
                } else {
                    if (!append_text(replacement, sizeof(replacement), &replacement_length, "[") ||
                        !append_text(replacement, sizeof(replacement), &replacement_length, reg) ||
                        !append_text(replacement, sizeof(replacement), &replacement_length, "]")) return -1;
                }

                /* Build new string */
                size_t prefix_len = (size_t) (offset_start - result);
                char new_result[CONVERTER_WORK_SIZE];
                strncpy(new_result, result, prefix_len);
                new_result[prefix_len] = '\0';
                strcat(new_result, replacement);
                strcat(new_result, close_paren + 1); /* Skip ) */

                strcpy(final, new_result);
                paren = final + prefix_len + strlen(replacement);
            } else {
                paren++;
            }
        }
    }

    /* Reverse operands for 2-operand instructions (AT&T: src, dest -> Intel: dest, src) */
    char *operand_start = insn_start;
    while (*operand_start && *operand_start != ' ' && *operand_start != '\t') operand_start++;
    while (*operand_start == ' ' || *operand_start == '\t') operand_start++;

    /* Find comma separating operands */
    char *comma = strchr(operand_start, ',');
    if (comma && *operand_start) {
        /* Extract first operand (source in AT&T) */
        char op1[CONVERTER_WORK_SIZE];
        size_t op1_len = (size_t) (comma - operand_start);
        while (op1_len > 0 && (operand_start[op1_len - 1] == ' ' || operand_start[op1_len - 1] == '\t')) op1_len--;
        strncpy(op1, operand_start, op1_len);
        op1[op1_len] = '\0';

        /* Extract second operand (dest in AT&T) */
        char op2[CONVERTER_WORK_SIZE];
        char *op2_start = comma + 1;
        while (*op2_start == ' ' || *op2_start == '\t') op2_start++;
        char *op2_end = op2_start;
        while (*op2_end && *op2_end != '\n' && *op2_end != '#' && *op2_end != ';') op2_end++;
        while (op2_end > op2_start && (op2_end[-1] == ' ' || op2_end[-1] == '\t' || op2_end[-1] == '\n')) op2_end--;
        size_t op2_len = (size_t) (op2_end - op2_start);
        strncpy(op2, op2_start, op2_len);
        op2[op2_len] = '\0';

        /* Rebuild instruction with reversed operands */
        char reversed[CONVERTER_WORK_SIZE];
        size_t prefix_len = (size_t) (operand_start - final);
        strncpy(reversed, final, prefix_len);
        reversed[prefix_len] = '\0';

        /* Add reversed operands: dest, src */
        /* For movsx/movzx with memory operands, add size qualifier */
        if (src_size_qualifier != NULL && op1[0] == '[') {
            /* op1 is a memory reference, add size qualifier */
            snprintf(reversed + prefix_len, sizeof(reversed) - prefix_len, "%s, %s%s", op2, src_size_qualifier, op1);
        } else {
            snprintf(reversed + prefix_len, sizeof(reversed) - prefix_len, "%s, %s", op2, op1);
        }

        /* Add any trailing content (comments, newlines) */
        if (*op2_end) {
            strcat(reversed, op2_end);
        }

        strcpy(final, reversed);
    }

    /* Copy final result */
    if (strlen(final) >= output_size) return -1;
    strncpy(output, final, output_size - 1);
    output[output_size - 1] = '\0';

    return 1;
}
