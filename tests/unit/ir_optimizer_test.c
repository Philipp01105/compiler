#include "test_source.h"
#include "ir_optimize.h"
#include "ast_optimize.h"
#include "errorHandler.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"IR optimizer check failed at %d: %s\n",__LINE__,#x); return 1; } } while (0)

static size_t count(const IrFunction *f, IrOpcode opcode) {
    size_t n = 0;
    for (size_t i = 0; i < f->instruction_count; ++i) if (f->instructions[i].opcode == opcode) ++n;
    return n;
}

static size_t count_binary(const IrFunction *f, TokenType operator_type) {
    size_t result = 0;
    for (size_t i = 0; i < f->instruction_count; i++)
        if (f->instructions[i].opcode == IR_OP_BINARY &&
            f->instructions[i].operator_type == operator_type)
            result++;
    return result;
}

static IrFunction *named(IrModule *m, const char *name) {
    for (size_t i = 0; i < m->function_count; ++i)
        if (!strcmp(ast_program_lexeme(m->functions[i].source_program, m->functions[i].name_token), name))
            return &m->
                    functions[i];
    return NULL;
}

static const IrInstruction *returned(IrFunction *f) {
    size_t value = IR_VALUE_NONE;
    for (size_t i = 0; i < f->instruction_count; ++i)
        if (f->instructions[i].opcode == IR_OP_RETURN)
            value = f->instructions[i].operand_a;
    for (size_t i = 0; i < f->instruction_count; ++i)
        if (f->instructions[i].result == value)
            return &f->instructions[
                i];
    return NULL;
}

int main(void) {
    ErrorHandler *errors = error_handler_init();
    CHECK(errors);
    error_handler_set_global(errors);
    error_handler_set_buffered(errors, 1);
    const char *source =
            "pub func effect() -> bit { return true; }"
            "pub func folded() -> int { var x = (2+3)*8; if (x == 40) { return x+2; } return 0; }"
            "pub func copy(x:int) -> int { var y=x; return y+0; }"
            "pub func stores(x:int) -> int { var y=0; y=x; y=x+1; return y; }"
            "pub func dead() -> int { var x=1; x=2; return 7; }"
            "pub func join(c:bit) -> int { var x=1; if(c) { x=4; } else { x=9; } return x; }"
            "pub func loop(limit:int) -> int { var x=0; while(x<limit) { x+=1; } return x; }"
            "pub func shorted() -> bit { return false && effect(); }"
            "pub func address() -> double { var x:double=1.25; var p:*double = (&x).(*double); x=3.5; return *p; }"
            "pub func reads(p:*int) -> int { return *p + *p; }"
            "pub func change(p:*int) -> int { var x=*p; *p+=1; return x+*p; }"
            "pub func escaped() -> int { var x=1; change((&x).(*int)); return x; }"
            "pub func trap() -> int { var unused=10/-1; return 0; }"
            "pub func bounds(i:int) -> int { var a:int[4]; var unused=a[i]; return 0; }"
            "pub func floats(x:double) -> double { return x*0.0; }"
            "pub func wrap() -> int { return 10+1; }"
            "pub func ast_branch() -> int { if(true) { return 3; } else { return 4; } return 5; }"
            "pub func ast_loop() -> int { while(false) { return 9; } return 6; }"
            "pub func counted() -> int { var i=0; var sum=0; while(i<1000) { sum+=3; i+=1; } return sum+i; }"
            "pub func ast_numeric() -> int { if(0) { return 1; } return 2; }"
            "pub func same(a:int,b:int) -> int { var x=a*b; var y=a*b; return x+y; }"
            "pub func across(a:int,b:int,c:bit) -> int { var x=a*b; if(c) { var y=a*b; return x+y; } return x; }"
            "pub func invariant(n:int,scale:int) -> int { var i=0; var sum=0; while(i<n) { sum+=scale*3; i+=1; } return sum; }"
            "pub func twice(i:int) -> int { var a:int[4]; return a[i]+a[i]; }"
            "pub func thread(c:bit) -> int { if(c) { } else { } return 1; }"
            "func unused_private() -> int { return 99; }"
            "pub func main() -> int { return folded(); }";
    FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source), "ir-optimizer-test.dmm", &options);
    CHECK(program);
    if (!program->structured_ast_complete) error_handler_flush(errors);
    CHECK(program->structured_ast_complete);
    SemanticModel *semantics = semantic_analyze(program);
    if (semantics && semantics->error_count) error_handler_flush(errors);
    CHECK(semantics && !semantics->error_count);
    AstOptimizationStats ast_stats = {0};
    ast_optimize_program(program, &ast_stats);
    CHECK(ast_stats.constant_branches >= 3 && ast_stats.constant_loops >= 1 && ast_stats.short_circuits >= 1 &&
        ast_stats.unreachable_statements >= 1);
    IrModule *m = ir_lower_program(program, semantics);
    CHECK(m && ir_verify_module(m));
    /* Exercise qword wraparound and the x86 signed division overflow trap
       without requiring source literals outside the frontend's literal range. */
    IrFunction *trap = named(m, "trap"), *wrap = named(m, "wrap");
    CHECK(trap && wrap);
    for (size_t i = 0; i < trap->instruction_count; ++i)
        if (trap->instructions[i].opcode == IR_OP_CONSTANT &&
            !strcmp(ast_program_lexeme(program, trap->instructions[i].auxiliary_token), "10")) {
            trap->instructions[i].has_immediate = 1;
            trap->instructions[i].immediate = UINT64_C(0x8000000000000000);
        }
    for (size_t i = 0; i < wrap->instruction_count; ++i)
        if (wrap->instructions[i].opcode == IR_OP_CONSTANT &&
            !strcmp(ast_program_lexeme(program, wrap->instructions[i].auxiliary_token), "10")) {
            wrap->instructions[i].has_immediate = 1;
            wrap->instructions[i].immediate = UINT64_MAX;
        }
    IrOptimizationStats stats;
    CHECK(ir_optimize_module(m,&stats) && m->verified && ir_verify_module(m));
    IrFunction *f = named(m, "folded");
    CHECK(f);
    CHECK(!count(f,IR_OP_BINARY) && !count(f,IR_OP_LOAD) && !count(f,IR_OP_BRANCH));
    CHECK(returned(f) && returned(f)->has_immediate && returned(f)->immediate == 42);
    f = named(m, "copy");
    CHECK(!count(f,IR_OP_DECLARE) && !count(f,IR_OP_BINARY) && count(f,IR_OP_LOAD) == 1);
    f = named(m, "stores");
    CHECK(count(f,IR_OP_STORE) == 1);
    f = named(m, "dead");
    CHECK(!count(f,IR_OP_DECLARE) && !count(f,IR_OP_STORE));
    f = named(m, "join");
    CHECK(count(f,IR_OP_BRANCH) == 1 && count(f,IR_OP_STORE) == 2);
    f = named(m, "loop");
    CHECK(count(f,IR_OP_BRANCH) == 1 && count(f,IR_OP_STORE) == 1);
    f = named(m, "shorted");
    CHECK(!count(f,IR_OP_CALL) && !count(f,IR_OP_PHI) && !count(f,IR_OP_BRANCH));
    CHECK(returned(f) && returned(f)->opcode == IR_OP_CONSTANT &&
        (returned(f)->has_immediate ? returned(f)->immediate == 0 :
            !strcmp(ast_program_lexeme(program, returned(f)->auxiliary_token), "false")));
    f = named(m, "address");
    CHECK(returned(f) && returned(f)->has_immediate);
    double value = 0;
    uint64_t bits = returned(f)->immediate;
    memcpy(&value, &bits, 8);
    CHECK(value == 3.5);
    f = named(m, "reads");
    CHECK(count(f,IR_OP_UNARY) == 1);
    f = named(m, "change");
    CHECK(count(f,IR_OP_UNARY) >= 2 && count(f,IR_OP_STORE) == 1);
    f = named(m, "escaped");
    CHECK(count(f,IR_OP_DECLARE) == 1 && count(f,IR_OP_LOAD) >= 2);
    CHECK(count(trap,IR_OP_BINARY) == 1 && count(named(m,"bounds"),IR_OP_INDEX) == 1);
    CHECK(count(named(m,"floats"),IR_OP_BINARY) == 1);
    CHECK(returned(wrap) && returned(wrap)->has_immediate && returned(wrap)->immediate == 0);
    CHECK(stats.constants_folded && stats.constants_propagated && stats.copies_propagated &&
        stats.dead_instructions && stats.dead_stores && stats.branches_folded && stats.blocks_removed && stats.
        addresses_simplified);
    CHECK(stats.common_expressions && stats.loop_invariants_hoisted && stats.dead_functions &&
        stats.bounds_checks_reused && stats.jumps_threaded);
    CHECK(named(m,"unused_private") == NULL);
    CHECK(count_binary(named(m,"across"), TOKEN_STAR) == 1);
    CHECK(!count(named(m,"ast_branch"),IR_OP_BRANCH));
    CHECK(!count(named(m,"ast_loop"),IR_OP_BRANCH));
    CHECK(!count(named(m,"counted"),IR_OP_BRANCH));
    CHECK(returned(named(m,"counted")) && returned(named(m,"counted"))->has_immediate &&
        returned(named(m,"counted"))->immediate == 4000);
    CHECK(ir_optimize_module(m,&stats));
    CHECK(!stats.constants_folded && !stats.constants_propagated && !stats.copies_propagated &&
        !stats.dead_instructions && !stats.dead_stores && !stats.branches_folded && !stats.blocks_removed && !stats.
        addresses_simplified);
    CHECK(!stats.common_expressions && !stats.bounds_checks_reused && !stats.jumps_threaded &&
        !stats.loop_invariants_hoisted && !stats.dead_functions);
    IrFunction *twice = named(m, "twice");
    CHECK(twice && count(twice, IR_OP_INDEX) >= 1);
    size_t checked_index = IR_VALUE_NONE;
    for (size_t i = 0; i < twice->instruction_count; i++)
        if (twice->instructions[i].opcode == IR_OP_INDEX && !twice->instructions[i].bounds_check_elided) {
            checked_index = i;
            break;
        }
    CHECK(checked_index != IR_VALUE_NONE);
    twice->instructions[checked_index].bounds_check_elided = 1;
    CHECK(!ir_verify_module(m));
    twice->instructions[checked_index].bounds_check_elided = 0;
    CHECK(ir_verify_module(m));
    IrInstruction *bad = &named(m, "main")->instructions[0];
    IrInstruction saved = *bad;
    bad->has_immediate = 2;
    CHECK(!ir_verify_module(m) && !ir_optimize_module(m,NULL));
    *bad = saved;
    CHECK(ir_verify_module(m));
    ir_module_free(m);
    semantic_model_free(semantics);
    ast_program_free(program);
    error_handler_free(errors);
    return 0;
}
