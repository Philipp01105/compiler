#include "frontend.h"
#include "ir.h"
#include "ir_optimize.h"
#include "errorHandler.h"
#include "generics.h"
#include <string.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"foundation check failed: %s at %d\n",#x,__LINE__); return 1; } } while (0)
int main(void) {
    ErrorHandler *handler=error_handler_init(); CHECK(handler != NULL);
    FILE *capture=tmpfile(); CHECK(capture != NULL);
    handler->output_stream=capture; error_handler_set_global(handler);
    const char *cache_source="func id<T>(x:T) -> T { return x; } func main() -> int { return 0; } func letter() -> char { return 'a'; }";
    AstProgram *a=frontend_parse_source(cache_source,strlen(cache_source),"cache.dmm",NULL);
    AstProgram *b=frontend_parse_source(cache_source,strlen(cache_source),"cache.dmm",NULL);
    CHECK(a && b);
    AstType integer=a->root->next->as.function.return_type;
    AstType character=a->root->next->next->as.function.return_type;
    AstDeclarationNode *ai=ast_specialize_function(a,a->root,&integer,1,a);
    AstDeclarationNode *ac=ast_specialize_function(a,a->root,&character,1,a);
    AstDeclarationNode *bc=ast_specialize_function(b,b->root,&character,1,a);
    AstDeclarationNode *bi=ast_specialize_function(b,b->root,&integer,1,a);
    CHECK(ai && ac && bi && bc);
    CHECK(ai == ast_specialize_function(a,a->root,&integer,1,a));
    CHECK(strcmp(ai->specialization_identity,bi->specialization_identity) == 0);
    CHECK(strcmp(ac->specialization_identity,bc->specialization_identity) == 0);
    CHECK(strcmp(ai->specialization_identity,ac->specialization_identity) != 0);
    ast_program_free(a); ast_program_free(b);
    const char *source="enum O<T> { Some(T), None, } func id<T>(x:T) -> T { return x; } "
        "func unwrap<T>(x:O<T>,fallback:T) -> T { match(x) { Some(v) => return v; None => return fallback; } } "
        "func make<T>(x:T) -> int { var o:O<T> = O<T>.Some(x); o=O<T>.None; match(o) { Some(v) => return 1; None => return 0; } } "
        "func main() -> int { var a:O<int> = O<int>.Some(id(42)); make('a'); return unwrap(a,id(7)); }";
    AstProgram *program=frontend_parse_source(source,strlen(source),"foundation.dmm",NULL);
    CHECK(program && ast_validate_program(program));
    SemanticModel *model=semantic_analyze(program); CHECK(model && model->error_count == 0);
    CHECK(ast_validate_program(program));
    size_t identities=0;
    for (AstDeclarationNode *d=program->root; d; d=d->next)
        if (d->generic_origin) { CHECK(d->specialization_identity != NULL); identities++; }
    CHECK(identities == 5);
    IrModule *module=ir_lower_program(program,model); CHECK(module && ir_verify_module(module));
    IrInstruction *payload=NULL, *guard=NULL;
    for (size_t f=0; f<module->function_count; f++)
        for (size_t i=0; i<module->functions[f].instruction_count; i++) {
            IrInstruction *in=&module->functions[f].instructions[i];
            if (in->opcode == IR_OP_ENUM_PAYLOAD) payload=in;
            if (in->opcode == IR_OP_ENUM_IS && !guard) guard=in;
        }
    CHECK(payload && guard);
    IrInstruction saved=*payload;
    payload->enum_payload_index=100; CHECK(!ir_verify_module(module)); *payload=saved;
    payload->target_a=IR_VALUE_NONE; CHECK(!ir_verify_module(module)); *payload=saved;
    payload->symbol_id=AST_SYMBOL_NONE; CHECK(!ir_verify_module(module)); *payload=saved;
    IrInstruction guard_saved=*guard;
    guard->opcode=IR_OP_LOAD; CHECK(!ir_verify_module(module)); *guard=guard_saved;
    CHECK(ir_optimize_module(module,NULL) && ir_verify_module(module));
    ir_module_free(module); semantic_model_free(model);
    model=semantic_analyze(program); CHECK(model && model->error_count == 0);
    module=ir_lower_program(program,model); CHECK(module && ir_verify_module(module));
    ir_module_free(module); semantic_model_free(model); ast_program_free(program);
    error_handler_free(handler); error_handler_set_global(NULL); fclose(capture); return 0;
}
