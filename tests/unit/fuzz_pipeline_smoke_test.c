#include "fuzz_pipeline.h"

#include <stdint.h>
#include <string.h>

static int exercise(const char *source) {
    const uint8_t *data = (const uint8_t *) source;
    size_t size = strlen(source);
    return dmm_fuzz_pipeline_input(data, size, DMM_FUZZ_PARSER) ||
           dmm_fuzz_pipeline_input(data, size, DMM_FUZZ_SEMANTIC) ||
           dmm_fuzz_pipeline_input(data, size, DMM_FUZZ_IR);
}

int main(void) {
    if (exercise("")) return 1;
    if (exercise("func main() -> int { const count:int=1+2; var values:int[count];"
        "values[1]=(4.5).(int); return values[1]; }"))
        return 2;
    if (exercise("struct Pair { var x:int; var y:int; } func main() -> void {"
        "var p:Pair; p.x = 4; var result:int=p.x; }"))
        return 3;
    if (exercise("func main( -> { while(true) { var x:[0]int;")) return 4;
    const uint8_t binary[] = {0, 0xff, '{', '}', '\n'};
    for (int stage = DMM_FUZZ_PARSER; stage <= DMM_FUZZ_IR; stage++)
        if (dmm_fuzz_pipeline_input(binary, sizeof(binary), (DmmFuzzStage) stage)) return 5;
    return 0;
}
