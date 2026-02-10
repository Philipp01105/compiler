#ifndef SYNTAX_CONVERTER_H
#define SYNTAX_CONVERTER_H

#include <stddef.h>
#include "compiler_types.h"

/*
 * Syntax Converter - Automatic AT&T to Intel Syntax Conversion
 * 
 * This module provides automatic conversion of AT&T syntax assembly
 * instructions to Intel syntax. It's designed to work transparently
 * with existing code generation that outputs AT&T syntax.
 */

/*
 * convert_att_to_intel - Convert AT&T syntax line to Intel syntax
 * @input: AT&T syntax assembly line
 * @output: Buffer for Intel syntax output
 * @output_size: Size of output buffer
 * @returns: 1 if conversion was performed, 0 if line was copied as-is
 * 
 * This function detects AT&T syntax patterns and converts them to Intel syntax.
 * It handles:
 * - Register names (% prefix removal)
 * - Immediate values ($ prefix removal)
 * - Operand order reversal (AT&T: src, dest → Intel: dest, src)
 * - Memory addressing syntax
 * - Instruction suffixes (q, l, w, b)
 */
int convert_att_to_intel(const char *input, char *output, size_t output_size);

#endif /* SYNTAX_CONVERTER_H */
