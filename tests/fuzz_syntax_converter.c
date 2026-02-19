#include "syntax_converter.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size >= 4096) return 0;
    char *input = malloc(size + 1);
    if (input == NULL) return 0;
    for (size_t i = 0; i < size; ++i) input[i] = data[i] == 0 ? ' ' : (char) data[i];
    input[size] = '\0';
    char output[4096];
    (void) convert_att_to_intel(input, output, sizeof(output));
    free(input);
    return 0;
}
