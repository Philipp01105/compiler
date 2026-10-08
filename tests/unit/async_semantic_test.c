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
            size_t entry=function->async_cancel_entry;
            function->async_cancel_entry=IR_VALUE_NONE;
            if(ir_verify_module(ir)) ok=0;
            function->async_cancel_entry=entry;
            for (size_t i = 0; i < function->instruction_count; i++) {
                IrInstruction *in = &function->instructions[i];
                if(in->opcode==IR_OP_CANCEL_AWAIT || in->opcode==IR_OP_CANCEL_DROP || in->opcode==IR_OP_CANCEL_RETURN) {
                    in->async_cleanup=0; if(ir_verify_module(ir)) ok=0; in->async_cleanup=1;
                    if(in->opcode!=IR_OP_CANCEL_RETURN) {
                        size_t state=in->target_a; in->target_a=0; if(ir_verify_module(ir)) ok=0; in->target_a=state;
                    }
                }
                if (in->opcode != IR_OP_AWAIT) continue;
                if(!in->async_cleanup) {
                    size_t edge=in->target_b; in->target_b=IR_VALUE_NONE; if(ir_verify_module(ir)) ok=0; in->target_b=edge;
                }
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
        {"struct Owner {var value:int;async func read()->int{return self.value;}} async func send_parent()->int{var owner=Owner{value:7};return owner.read().await();} func main()->int{var handle=spawn(send_parent());block_on(cancel(handle));return 0;}",NULL},
        {"struct Owner {var value:int;async func read()->int{return self.value;}} func main()->int{var owner=Owner{value:7};var handle=spawn(owner.read());block_on(cancel(handle));return 0;}","spawn requires a Send Future frame"},
        {"struct Owner {var value:int;async func read()->int{return self.value;}} async func parent(owner:&Owner)->int{return owner.read().await();} func main()->int{var owner=Owner{value:7};var handle=spawn(parent(&owner));block_on(cancel(handle));return 0;}","spawn requires a Send Future frame"},
        {"async func work(x:&int) -> int { return *x; } async func wrap(x:&int) -> Result<Future<int>,TaskError> { return Result<Future<int>,TaskError>.Ok(work(x)); } func main() -> int { var x=7; var r=block_on(wrap(&x)); match(r) { Ok(f) => { var c=cancel(f); x=8; block_on(c); } Err(e) => {} } return x; }","while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } async func wrap(x:&int) -> Result<Future<int>,TaskError> { return Result<Future<int>,TaskError>.Ok(work(x)); } func main() -> int { var x=7; var r=block_on(wrap(&x)); match(r) { Ok(f) => { block_on(cancel(f)); } Err(e) => {} } x=8; return x; }",NULL},
        {"async func work(x:&int) -> int { return *x; } func wrap(x:&int) -> Result<Future<int>,TaskError> { return Result<Future<int>,TaskError>.Ok(work(x)); } func main() -> int { var x=7; var r=wrap(&x); match(r) { Ok(f) => { var c=cancel(f); x=8; block_on(c); } Err(e) => {} } return x; }","while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } func relay(r:Result<Future<int>,TaskError>) -> Result<Future<int>,TaskError> { return r; } func main() -> int { var x=7; var r=relay(Result<Future<int>,TaskError>.Ok(work(&x))); match(r) { Ok(f) => { block_on(cancel(f)); } Err(e) => {} } x=8; return x; }",NULL},
        {"async func work(x:&int) -> int { return *x; } func bad() -> Result<Future<int>,TaskError> { var x=7; return Result<Future<int>,TaskError>.Ok(work(&x)); } func main() -> int { return 0; }","cannot capture a borrow of a local value"},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var r=Result<Future<int>,TaskError>.Ok(work(&x)); var s=r; match(s) { Ok(f) => { block_on(cancel(f)); } Err(e) => {} } x=8; return x; }",NULL},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var r=Result<Future<int>,TaskError>.Ok(work(&x)); x=8; match(r) { Ok(f) => return block_on(f); Err(e) => return 0; } }","while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var r=Result<Future<int>,TaskError>.Ok(work(&x)); match(r) { Ok(f) => { var c=cancel(f); x=8; block_on(c); } Err(e) => {} } return x; }","while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var r=Result<Future<int>,TaskError>.Ok(work(&x)); match(r) { Ok(f) => { block_on(cancel(f)); x=8; } Err(e) => { x=9; } } x=10; return x; }",NULL},
        {"async func work() -> int { return 7; } async func join(h:JoinHandle<int>) -> Result<int,TaskError> { return h.await(); } func main() -> int { var e=Executor.create(2); var h=e.spawn(work()); var r=e.block_on(join(h)); block_on(e.shutdown(ShutdownMode.Drain)); return 0; }",NULL},
        {"async func work() -> int { return 7; } async func nested() -> Future<int> { return work(); } async func join(h:JoinHandle<Future<int>>) -> int { var r=h.await(); match(r) { Ok(f) => return f.await(); Err(e) => return 0; } } func main() -> int { return block_on(join(spawn(nested()))); }",NULL},
        {"async func work() -> int { return 7; } async func nested() -> Future<int> { return work(); } async func join(h:JoinHandle<Future<int>>) -> int { var r=h.await(); match(r) { Ok(f) => return 0; Err(e) => return 0; } } func main() -> int { return block_on(join(spawn(nested()))); }","implicit drop is forbidden"},
        {"async func work() -> void { return; } async func join(h:JoinHandle<void>) -> void { var r:Result<void,TaskError>=h.await(); match(r) { Ok => return; Err(e) => return; } } func main() -> int { block_on(join(spawn(work()))); return 0; }",NULL},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var f=work(&x); var n=block_on(f); x=8; return n; }",NULL},
        {"async func work(x:&int) -> &int { return x; } func main() -> int { var x=7; var f=work(&x); var r=block_on(f); x=8; return *r; }","while it is borrowed"},
        {"async func work(x:&int) -> &int { return x; } func main() -> int { var x=7; var f=work(&x); var r=block_on(f); var n=*r; x=8; return n; }",NULL},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var f=work(&x); var c=cancel(f); x=8; block_on(c); return 0; }","while it is borrowed"},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var f=work(&x); var c=cancel(f); block_on(c); x=8; return x; }",NULL},
        {"async func work(x:&int) -> int { return *x; } func main() -> int { var x=7; var h=spawn(work(&x)); block_on(cancel(h)); return 0; }","spawn requires a Send Future frame"},
        {"async func work(x:*int) -> int { return *x; } func main() -> int { var p:*int; var h=spawn(work(p)); block_on(cancel(h)); return 0; }","spawn requires a Send Future frame"},
        {"async func work() -> int { return 7; } async func bad() -> int { return block_on(work()); } func main() -> int { return 0; }","block_on is only valid"},
        {"func main() -> int { var e=Executor.create(0); block_on(e.shutdown(ShutdownMode.Cancel)); return 0; }","at least one worker"},
        {"func main() -> int { var e=Executor.create(1); return 0; }","implicit drop is forbidden"},
        {"func main() -> int { Executor.create(1); return 0; }","implicit drop is forbidden"},
        {"async func work() -> int { return 7; } func main() -> int { spawn(work()); return 0; }","implicit drop is forbidden"},
        {"async func work() -> int { return 7; } func main() -> int { var h=spawn(work()); return 0; }","implicit drop is forbidden"},
        {"async func work() -> int { return 7; } func main() -> int { var h=spawn(work()); var c=cancel(h); return 0; }","implicit drop is forbidden"},
        {"func bad(b:bit) -> int { var e=Executor.create(1); if(b) { block_on(e.shutdown(ShutdownMode.Drain)); return 0; } return 1; } func main() -> int { return 0; }","implicit drop is forbidden"},
        {"func main() -> int { var e=Executor.create(1); block_on(e.shutdown(ShutdownMode.Drain)); block_on(e.shutdown(ShutdownMode.Cancel)); return 0; }","after ownership was moved"},
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
