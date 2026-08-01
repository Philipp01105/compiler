#include "test_source.h"
#include "ir.h"
#include "errorHandler.h"
#include <stdio.h>
#include <string.h>

static int check_case(const char *source, const char *diagnostic, int enabled) {
    ErrorHandler *handler = error_handler_init();
    FILE *capture = tmpfile();
    if (handler == NULL || capture == NULL) return 0;
    handler->output_stream = capture;
    error_handler_set_global(handler);
    AstProgram *program = test_parse_source(source, strlen(source), "async.dmm", NULL);
    int ok = program != NULL && ast_validate_program(program);
    if (ok && enabled) {
        DmmModule *module = ast_program_alloc(program, sizeof(*module));
        DmmFeature *feature = ast_program_alloc(program, sizeof(*feature));
        ok = module != NULL && feature != NULL;
        if (ok) {
            feature->name = "async";
            module->features = feature;
            program->module = module;
        }
    }
    SemanticModel *model = ok ? semantic_analyze(program) : NULL;
    ok = ok && model != NULL && ast_validate_program(program);
    if (ok) ok = diagnostic == NULL ? model->error_count == 0 : model->error_count != 0;
    char output[16384];
    error_handler_flush(handler);
    rewind(capture);
    size_t count = fread(output, 1, sizeof(output) - 1, capture);
    output[count] = '\0';
    if (diagnostic != NULL && strstr(output, diagnostic) == NULL) ok = 0;
    /* Typed async source must lower to verified suspendable functions. */
    if (ok && diagnostic == NULL && strstr(source, "async func") != NULL) {
        IrModule *ir = ir_lower_program(program, model);
        if (ir == NULL || !ir_verify_module(ir)) ok = 0;
        if (ir != NULL) for (size_t f = 0; f < ir->function_count; f++) {
            IrFunction *function = &ir->functions[f];
            const char *name = ast_program_lexeme(function->source_program, function->name_token);
            if (strncmp(name, "send_", 5) == 0 && !(function->async_frame_properties & SEMANTIC_TYPE_SEND)) ok = 0;
            if (strncmp(name, "nosend_", 7) == 0 && (function->async_frame_properties & SEMANTIC_TYPE_SEND)) ok = 0;
            if (!function->is_async) continue;
            function->async_frame_pinned = 0;
            if (ir_verify_module(ir)) ok = 0;
            function->async_frame_pinned = 1;
            for (size_t i = 0; i < function->instruction_count; i++) {
                IrInstruction *in = &function->instructions[i];
                if (in->opcode != IR_OP_AWAIT) continue;
                size_t state = in->target_a;
                in->target_a = function->async_state_count + 1;
                if (ir_verify_module(ir)) ok = 0;
                in->target_a = state;
                for (size_t j = 0; j < i; j++) {
                    const IrInstruction *prior = &function->instructions[j];
                    if (prior->opcode != IR_OP_AWAIT || prior->type_id != in->type_id) continue;
                    in->target_a = prior->target_a;
                    if (ir_verify_module(ir)) ok = 0;
                    in->target_a = state;
                    if (!strcmp(name, "verify_effects")) {
                        IrInstruction saved = *in;
                        in->operand_a = prior->operand_a;
                        if (ir_verify_module(ir)) ok = 0;
                        *in = saved;
                        IrInstruction saved_prior = function->instructions[j];
                        const IrInstruction *child = NULL;
                        for (size_t p = 0; p < j; p++)
                            if (function->instructions[p].result == prior->operand_a)
                                child = &function->instructions[p];
                        if (child != NULL) {
                            function->instructions[j].opcode = IR_OP_DROP;
                            function->instructions[j].type_id = child->type_id;
                            in->opcode = IR_OP_DROP;
                            in->type_id = child->type_id;
                            in->operand_a = prior->operand_a;
                            if (ir_verify_module(ir)) ok = 0;
                        }
                        function->instructions[j] = saved_prior;
                        *in = saved;
                    }
                }
            }
        }
        ir_module_free(ir);
    }
    if (!ok) fprintf(stderr, "Async semantic case failed:\n%s\n%s\n", source, output);
    semantic_model_free(model);
    ast_program_free(program);
    error_handler_free(handler);
    error_handler_set_global(NULL);
    fclose(capture);
    return ok;
}

int main(void) {
    static const struct { const char *source; const char *diagnostic; } cases[] = {
        {"async func work() -> int { return 7; } async func use() -> int { var x:Future<int>=work(); var y=x.await(); return y; } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func nested() -> Future<int> { return work(); } async func use() -> int { return nested().await().await(); } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func use() -> int { var x=work(); var y=x.await(); return x.await(); } func main() -> int { return 0; }", "after ownership was moved"},
        {"async func work() -> int { return 7; } func main() -> int { return work().await(); }", "await is only valid inside an async function"},
        {"async func use() -> int { var n=7; return n.await(); } func main() -> int { return 0; }", "await requires an owned Future"},
        {"async func work() -> int { return 7; } async func use() -> int { var f=work(); return (&f).await(); } func main() -> int { return 0; }", "await requires an owned Future"},
        {"struct S { var r:&int; } async func work(s:S) -> int { return *s.r; } async func use() -> int { var x=7; var s:S; s.r=&x; var f=work(s); x=8; return f.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"struct S { var x:int; } async func send_aggregate(s:S) -> S { return s; } struct P { var p:*int; } async func nosend_aggregate(p:P) -> P { return p; } func main() -> int { return 0; }", NULL},
        {"async func work() -> void { return; } async func verify_effects() -> void { work().await(); work().await(); } func main() -> int { return 0; }", NULL},
        {"enum B { A, B, } async func work(x:&int) -> int { return *x; } async func use(b:B) -> int { var x=7; var f=work(&x); match(b) { A => return f.await(); B => { x=8; return f.await(); } } } func main() -> int { return 0; }", "while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } async func use(b:bit) -> int { var x=7; var f=work(&x); while(b) { return f.await(); } x=8; return f.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"struct S { var x:int; async func read() -> int { return x; } } async func use() -> int { var s:S; var f=s.read(); s.x=8; return f.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"struct S { var x:int; async func read() -> int { return x; } } async func use() -> int { var s:S; var f=s.read(); var n=s.x; return f.await(); } func main() -> int { return 0; }", "while it is mutably borrowed"},
        {"struct S { var x:int; async func read() -> int { return x; } } async func use() -> int { var s:S; var f=s.read(); var n=f.await(); s.x=8; return n; } func main() -> int { return 0; }", NULL},
        {"async func send_plain(x:int) -> int { return x; } async func send_nested() -> int { return send_plain(7).await(); } func main() -> int { return 0; }", NULL},
        {"async func nosend_borrow(x:&int) -> int { return *x; } async func nosend_pointer(x:*int) -> int { return *x; } func main() -> int { return 0; }", NULL},
        {"async func send_work() -> int { return 7; } async func send_local() -> int { var x=7; var r=&x; var n=send_work().await(); return n+*r; } func main() -> int { return 0; }", NULL},
        {"async func send_work() -> int { return 7; } async func send_twice() -> int { var a=send_work().await(); var b=send_work().await(); return a+b; } func main() -> int { return 0; }", NULL},
        {"async func main() -> int { return 0; }", "main must have no parameters"},
        {"async func array() -> int[2] { return [7,8]; } async func use() -> int { var a=array().await(); return a[1]; } func main() -> int { return 0; }", NULL},
        {"struct W { var f:Future<int>; } func relay(w:W) -> W { return w; } func main() -> int { return 0; }", NULL},
        {"struct W { var f:Future<int>; } func relay(w:W) -> W { return w; } func bad(w:W) -> void { relay(w); } func main() -> int { return 0; }", "implicit drop is forbidden"},
        {"async func work() -> int { return 7; } async func use() -> int { var f=work(); defer func() { f.await(); } return 0; } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func use() -> int { defer work(); return 0; } func main() -> int { return 0; }", "deferred call cannot discard a Future"},
        {"async func work<T>(x:T) -> T { return x; } async func use() -> int { return work(7).await(); } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> int { return *x; } func relay(f:Future<int>) -> Future<int> { return f; } async func use() -> int { var x=7; var f=work(&x); var call:func(Future<int>)->Future<int>=relay; var n=call(f).await(); x=8; return n; } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> int { return *x; } func relay(f:Future<int>) -> Future<int> { return f; } async func use() -> int { var x=7; var f=relay(work(&x)); x=8; return f.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } async func use() -> int { var f:Future<int>; { var x=7; var g=work(&x); f=g; } return f.await(); } func main() -> int { return 0; }", "cannot outlive the origin"},
        {"async func work(x:&int) -> &int { return x; } async func use() -> int { var x=7; var f=work(&x); var r=f.await(); var n=*r; x=8; return n; } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> &int { return x; } async func use() -> int { var x=7; var f=work(&x); var r=f.await(); x=8; return *r; } func main() -> int { return 0; }", "while it is borrowed"},
        {"async func work() -> int { return 7; } func relay<T>(f:Future<T>) -> Future<T> { return f; } async func use() -> int { return relay(work()).await(); } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func nested() -> Future<int> { return work(); } async func use() -> int { return nested().await().await(); } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> int { return *x; } async func use(b:bit) -> int { var x=7; var f=work(&x); if(b) { f.await(); } else { f.await(); } x=8; return x; } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> int { return *x; } async func use(b:bit) -> int { var x=7; var f=work(&x); if(b) { return f.await(); } x=8; return f.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } func relay(f:Future<int>) -> Future<int> { return f; } async func use() -> int { var x=7; var f=work(&x); var g=relay(f); x=8; return g.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } async func use() -> int { var f:Future<int>; { var x=7; f=work(&x); } return f.await(); } func main() -> int { return 0; }", "cannot outlive the origin"},
        {"async func work(x:&int) -> int { return *x; } async func use() -> int { var x=7; var f=work(&x); var n=f.await(); x=8; return n; } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> int { return *x; } async func use() -> int { var x=7; var f=work(&x); var g=f; return g.await(); } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> int { return *x; } func relay(x:&int) -> Future<int> { return work(x); } func main() -> int { return 0; }", NULL},
        {"async func work(x:&int) -> int { return *x; } async func use() -> int { var x=7; var f=work(&x); x=8; return f.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } async func use() -> int { var x=7; var f=work(&x); var g=f; x=8; return g.await(); } func main() -> int { return 0; }", "while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } func bad() -> Future<int> { var x=7; return work(&x); } func main() -> int { return 0; }", "returned Future cannot capture"},
        {"async func work(x:&int) -> int { return *x; } func bad() -> Future<int> { var x=7; var f=work(&x); return f; } func main() -> int { return 0; }", "returned Future cannot capture"},
        {"async func work(x:&mut int) -> int { return *x; } async func use() -> int { var x=7; var f=work(&mut x); var n=x; return f.await(); } func main() -> int { return 0; }", "while it is mutably borrowed"},
        {"async func work() -> int { return 7; } async func use() -> int { return work().await(); } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func use() -> int { var f=work(); var g=f; return g.await(); } func main() -> int { return 0; }", NULL},
        {"async func work() -> void { return; } async func use() -> void { work().await(); } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } func relay(f:Future<int>) -> Future<int> { return f; } async func use() -> int { return relay(work()).await(); } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func use(b:bit) -> int { var f=work(); if(b) { return f.await(); } else { return f.await(); } } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func use() -> int { var call=work; return call().await(); } func main() -> int { return 0; }", NULL},
        {"async func work() -> int { return 7; } async func use() -> int { var f:Future<int>=work(); return f.await(); } func main() -> int { return 0; }", NULL},
        {"async func use() -> int { return (4).await(); } func main() -> int { return 0; }", "await requires an owned Future"},
        {"func main() -> int { return (4).await(); }", "await is only valid inside an async function"},
        {"async func work() -> int { return 7; } func main() -> int { return work(); }", "expected 'int'"},
        {"async func work() -> int { return 7; } async func use() -> int { var f=work(); var n=f.await(); return f.await(); } func main() -> int { return 0; }", "after ownership was moved"},
        {"async func work() -> int { return 7; } func main() -> int { var f=work(); return 0; }", "implicit drop is forbidden"},
        {"async func work() -> int { return 7; } func main() -> int { work(); return 0; }", "Future expression must be awaited or transferred"},
        {"async func work() -> int { return 7; } async func use() -> int { var f=work(); f=work(); return f.await(); } func main() -> int { return 0; }", "implicit drop is forbidden"},
        {"async func work() -> int { return 7; } async func use(b:bit) -> int { var f=work(); if(b) { return f.await(); } return 0; } func main() -> int { return 0; }", "implicit drop is forbidden"},
        {"async func work() -> int { return 7; } async func use() -> int { {var f=work();} return 0; } func main() -> int { return 0; }", "implicit drop is forbidden"},
        {"async func work() -> int { return 7; } async func use(b:bit) -> int { while(b) { var f=work(); break; } return 0; } func main() -> int { return 0; }", "implicit drop is forbidden"},
        {"async func work() -> int { return 7; } async func use() -> int { var f:Future<byte>=work(); return f.await(); } func main() -> int { return 0; }", "Future<int>"},
        {"async func work() -> int { return 7; } async func use() -> int { var f=work(); var r=&f; return r.await(); } func main() -> int { return 0; }", "await requires an owned Future"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!check_case(cases[i].source, cases[i].diagnostic, 1)) return 1;
    if (!check_case("async func work() -> int { return 7; } func main() -> int { return 0; }",
                    "async functions require the async manifest feature", 0)) return 1;
    return 0;
}
