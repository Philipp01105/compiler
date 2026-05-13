#include "fuzz_pipeline.h"

#include "errorHandler.h"
#include "frontend.h"
#include "ir.h"
#include "semantic.h"

#include <stdlib.h>

#define DMM_FUZZ_MAX_INPUT (64U * 1024U)

static ErrorHandler *fuzz_errors(void) {
    static ErrorHandler *handler;
    if (handler == NULL) {
        handler = error_handler_init();
        if (handler != NULL) {
            error_handler_set_buffered(handler, 1);
            error_handler_set_max_errors(handler, 128);
            error_handler_set_global(handler);
        }
    }
    return handler;
}

static size_t input_hash(const uint8_t *data, size_t size) {
    size_t hash = (size_t) 2166136261U;
    for (size_t i = 0; i < size; i++) hash = (hash ^ data[i]) * (size_t) 16777619U;
    return hash;
}

static void fuzz_ir_verifier(IrModule *module, const uint8_t *data, size_t size) {
    if (!ir_verify_module(module)) abort();
    if (module->function_count == 0) return;
    size_t hash = input_hash(data, size);
    IrFunction *function = &module->functions[hash % module->function_count];
    if (function->instruction_count == 0) return;
    IrInstruction *instruction = &function->instructions[
        (hash / module->function_count) % function->instruction_count];
    switch (hash % 8U) {
        case 0: instruction->opcode = (IrOpcode) 255; break;
        case 1: instruction->type_id = IR_TYPE_NONE; break;
        case 2: instruction->result = function->next_value; break;
        case 3: instruction->operand_a = function->next_value; break;
        case 4: instruction->operand_b = function->next_value; break;
        case 5: instruction->symbol_id = AST_SYMBOL_NONE; break;
        case 6: instruction->target_a = function->next_label; break;
        case 7: instruction->operator_type = TOKEN_ERROR; break;
    }
    (void) ir_verify_module(module);
}

int dmm_fuzz_pipeline_input(const uint8_t *data, size_t size, DmmFuzzStage stage) {
    ErrorHandler *errors = fuzz_errors();
    if (errors == NULL || data == NULL || size > DMM_FUZZ_MAX_INPUT) return 0;

    const FrontendOptions options = {0};
    AstProgram *program = frontend_parse_source((const char *) data, size, "<fuzz>", &options);
    if (program == NULL) {
        error_handler_reset(errors);
        return 0;
    }
    if (program->structured_ast_complete && !ast_validate_program(program)) abort();

    SemanticModel *semantics = NULL;
    IrModule *module = NULL;
    if (stage >= DMM_FUZZ_SEMANTIC) semantics = semantic_analyze(program);
    if (stage >= DMM_FUZZ_IR && semantics != NULL && semantics->error_count == 0) {
        module = ir_lower_program(program, semantics);
        if (module != NULL) fuzz_ir_verifier(module, data, size);
    }

    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    error_handler_reset(errors);
    return 0;
}
