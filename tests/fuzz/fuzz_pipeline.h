#ifndef DMM_FUZZ_PIPELINE_H
#define DMM_FUZZ_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    DMM_FUZZ_PARSER,
    DMM_FUZZ_SEMANTIC,
    DMM_FUZZ_IR
} DmmFuzzStage;

int dmm_fuzz_pipeline_input(const uint8_t *data, size_t size, DmmFuzzStage stage);

#endif
