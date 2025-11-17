#include "syntax_converter.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <regex.h>

/*
 * Simple pattern-based converter using string substitution
 */
int convert_att_to_intel(const char *input, char *output, size_t output_size) {
    /* Check if this looks like an AT&T instruction */
    const char *p = input;
    
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t') p++;
    
    /* Check for AT&T instruction patterns */
    int is_att = 0;
    if (strstr(p, "movq") || strstr(p, "movl") || strstr(p, "movw") || strstr(p, "movb") ||
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
        strstr(p, "orl") || strstr(p, "orq")) {
        is_att = 1;
    }
    
    if (!is_att) {
        /* Not AT&T syntax, copy as-is */
        strncpy(output, input, output_size - 1);
        output[output_size - 1] = '\0';
        return 0;
    }
    
    /* Simple conversion: Handle common patterns */
    char temp[1024];
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
        } else if (*src == '$' && (isdigit(src[1]) || src[1] == '-')) {
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
    char final[1024];
    strncpy(final, output, sizeof(final) - 1);
    final[sizeof(final) - 1] = '\0';
    
    /* Remove instruction suffixes (q, l, w, b) */
    char *insn_start = final;
    while (*insn_start == ' ' || *insn_start == '\t') insn_start++;
    
    if (strncmp(insn_start, "movq ", 5) == 0) memcpy(insn_start, "mov  ", 5);
    else if (strncmp(insn_start, "movl ", 5) == 0) memcpy(insn_start, "mov  ", 5);
    else if (strncmp(insn_start, "movw ", 5) == 0) memcpy(insn_start, "mov  ", 5);
    else if (strncmp(insn_start, "movb ", 5) == 0) memcpy(insn_start, "mov  ", 5);
    else if (strncmp(insn_start, "pushq ", 6) == 0) memcpy(insn_start, "push  ", 6);
    else if (strncmp(insn_start, "popq ", 5) == 0) memcpy(insn_start, "pop  ", 5);
    else if (strncmp(insn_start, "addq ", 5) == 0) memcpy(insn_start, "add  ", 5);
    else if (strncmp(insn_start, "addl ", 5) == 0) memcpy(insn_start, "add  ", 5);
    else if (strncmp(insn_start, "subq ", 5) == 0) memcpy(insn_start, "sub  ", 5);
    else if (strncmp(insn_start, "subl ", 5) == 0) memcpy(insn_start, "sub  ", 5);
    else if (strncmp(insn_start, "leaq ", 5) == 0) memcpy(insn_start, "lea  ", 5);
    else if (strncmp(insn_start, "leal ", 5) == 0) memcpy(insn_start, "lea  ", 5);
    else if (strncmp(insn_start, "xorl ", 5) == 0) memcpy(insn_start, "xor  ", 5);
    else if (strncmp(insn_start, "xorq ", 5) == 0) memcpy(insn_start, "xor  ", 5);
    else if (strncmp(insn_start, "cmpl ", 5) == 0) memcpy(insn_start, "cmp  ", 5);
    else if (strncmp(insn_start, "cmpq ", 5) == 0) memcpy(insn_start, "cmp  ", 5);
    else if (strncmp(insn_start, "testb ", 6) == 0) memcpy(insn_start, "test  ", 6);
    else if (strncmp(insn_start, "testl ", 6) == 0) memcpy(insn_start, "test  ", 6);
    else if (strncmp(insn_start, "testq ", 6) == 0) memcpy(insn_start, "test  ", 6);
    else if (strncmp(insn_start, "imull ", 6) == 0) memcpy(insn_start, "imul  ", 6);
    else if (strncmp(insn_start, "imulq ", 6) == 0) memcpy(insn_start, "imul  ", 6);
    else if (strncmp(insn_start, "idivl ", 6) == 0) memcpy(insn_start, "idiv  ", 6);
    else if (strncmp(insn_start, "idivq ", 6) == 0) memcpy(insn_start, "idiv  ", 6);
    else if (strncmp(insn_start, "negl ", 5) == 0) memcpy(insn_start, "neg  ", 5);
    else if (strncmp(insn_start, "negq ", 5) == 0) memcpy(insn_start, "neg  ", 5);
    else if (strncmp(insn_start, "incq ", 5) == 0) memcpy(insn_start, "inc  ", 5);
    else if (strncmp(insn_start, "decq ", 5) == 0) memcpy(insn_start, "dec  ", 5);
    else if (strncmp(insn_start, "andl ", 5) == 0) memcpy(insn_start, "and  ", 5);
    else if (strncmp(insn_start, "andq ", 5) == 0) memcpy(insn_start, "and  ", 5);
    else if (strncmp(insn_start, "orl ", 4) == 0) memcpy(insn_start, "or  ", 4);
    else if (strncmp(insn_start, "orq ", 4) == 0) memcpy(insn_start, "or  ", 4);
    
    /* Handle memory addressing: (reg) -> [reg],  offset(reg) -> [reg+offset] */
    /* Handle RIP-relative: label(rip) -> [rel label] */
    char *result = final;
    char *paren = result;
    while ((paren = strchr(paren, '(')) != NULL) {
        /* Check if this is (rip) */
        if (strncmp(paren, "(rip)", 5) == 0) {
            /* Find the label before ( */
            char *label_end = paren;
            char *label_start = label_end - 1;
            while (label_start > result && (isalnum(*label_start) || *label_start == '_' || *label_start == '.' || *label_start == '-')) {
                label_start--;
            }
            label_start++;
            
            /* Extract label */
            size_t label_len = label_end - label_start;
            char label[256];
            strncpy(label, label_start, label_len);
            label[label_len] = '\0';
            
            /* Replace with [rel label] */
            char replacement[512];
            snprintf(replacement, sizeof(replacement), "[rel %s]", label);
            
            /* Build new string */
            size_t prefix_len = label_start - result;
            char new_result[1024];
            strncpy(new_result, result, prefix_len);
            new_result[prefix_len] = '\0';
            strcat(new_result, replacement);
            strcat(new_result, paren + 5);  /* Skip (rip) */
            
            strcpy(final, new_result);
            paren = final + prefix_len + strlen(replacement);
        } else {
            /* Regular memory operand */
            paren++;
        }
    }
    
    /* Copy final result */
    strncpy(output, final, output_size - 1);
    output[output_size - 1] = '\0';
    
    return 1;
}
