#include "syntax_converter.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (data == NULL || size > 64U * 1024U) return 0;
    char *input = malloc(size + 1U);
    char *output = malloc(size * 2U + 128U);
    if (input != NULL && output != NULL) {
        memcpy(input, data, size);
        input[size] = '\0';
        (void) convert_att_to_intel(input, output, size * 2U + 128U);
    }
    free(output);
    free(input);
    return 0;
}
