#include "fuzz_pipeline.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return dmm_fuzz_pipeline_input(data, size, DMM_FUZZ_SEMANTIC);
}
