#include "ir_optimize.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef enum { UNKNOWN, BOTTOM, CONSTANT, VALUE } FactKind;
typedef struct { FactKind kind; uint64_t bits; size_t value; } Fact;
typedef struct { size_t begin, end, successor[2], predecessor; int reachable; } Block;
typedef struct { size_t block, next; } Edge;
typedef struct {
    Block *blocks; Edge *edges;
    size_t count, *owner, *labels, *definitions;
} Graph;
typedef struct {
    size_t symbol, declaration; IrTypeId type;
} Local;
typedef struct {
    IrModule *module; IrFunction *function; Graph graph;
    Local *locals; size_t local_count, *local_index;
    unsigned char *protected_values, *address_only, *remove;
    Fact *values, *incoming, *outgoing, *scratch;
    size_t *aliases;
    IrOptimizationStats *stats;
} Pass;

static Fact unknown(void) { return (Fact){UNKNOWN, 0, IR_VALUE_NONE}; }
static Fact bottom(void) { return (Fact){BOTTOM, 0, IR_VALUE_NONE}; }
static Fact literal(uint64_t bits) { return (Fact){CONSTANT, bits, IR_VALUE_NONE}; }
static Fact reference(size_t value) { return (Fact){VALUE, 0, value}; }
static int equal(Fact a, Fact b) {
    return a.kind == b.kind && (a.kind != CONSTANT || a.bits == b.bits) &&
        (a.kind != VALUE || a.value == b.value);
}
static Fact meet(Fact a, Fact b) {
    if (a.kind == BOTTOM) return b;
    if (b.kind == BOTTOM) return a;
    return equal(a,b) ? a : unknown();
}
static int floating(DataType type) { return type == TYPE_FLOAT || type == TYPE_DOUBLE; }
static int numeric(const IrModule *m, IrTypeId type) {
    DataType primitive = m->types[type].primitive;
    return m->types[type].kind == IR_TYPE_PRIMITIVE && (data_type_integral(primitive) || floating(primitive));
}
static int scalar(const IrModule *m, IrTypeId type) {
    return m->types[type].kind == IR_TYPE_POINTER ||
        (m->types[type].kind == IR_TYPE_PRIMITIVE && m->types[type].primitive < TYPE_VOID);
}
static int64_t signed_bits(uint64_t bits) { int64_t result; memcpy(&result,&bits,8); return result; }
static double number(Fact fact, DataType type) {
    if (type == TYPE_FLOAT) { uint32_t bits=(uint32_t)fact.bits; float value; memcpy(&value,&bits,4); return value; }
    if (type == TYPE_DOUBLE) { double value; memcpy(&value,&fact.bits,8); return value; }
    return data_type_unsigned(type) ? (double)fact.bits : (double)signed_bits(fact.bits);
}
static Fact real(double value, DataType type) {
    if (type == TYPE_FLOAT) { float single=(float)value; uint32_t bits; memcpy(&bits,&single,4); return literal(bits); }
    uint64_t bits; memcpy(&bits,&value,8); return literal(bits);
}
static int finite_number(double value) {
    uint64_t bits; memcpy(&bits,&value,8);
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}
static int truth(Fact fact, DataType type) { return floating(type) ? number(fact,type) != 0.0 : fact.bits != 0; }
static Fact convert(Fact fact, DataType from, DataType to, int cast) {
    if (fact.kind != CONSTANT) return unknown();
    if (floating(to)) {
        if (from != to) fact=real(number(fact,from),to);
    } else if (floating(from)) {
        double value=number(fact,from);
        if (to == TYPE_BIT && cast) return literal(value != 0);
        if (data_type_unsigned(to) && data_type_bytes(to) == 8) {
            if (!finite_number(value) || value < 0 || value >= 0x1p64) return unknown();
            fact=literal((uint64_t)value);
        } else {
            if (!finite_number(value) || value < -0x1p63 || value >= 0x1p63) return unknown();
            fact=literal((uint64_t)(int64_t)value);
        }
    }
    if (cast && to == TYPE_BIT) fact=literal(fact.bits != 0);
    if (cast && (to == TYPE_CHAR || to == TYPE_BYTE)) fact=literal(fact.bits & 255);
    if (data_type_fixed_integer(to)) fact=literal(data_type_normalize_integer(fact.bits,to));
    else if (data_type_fixed_integer(from) && to == TYPE_INT) fact=literal(data_type_normalize_integer(fact.bits,to));
    return fact;
}
static Fact fold_binary(TokenType op, Fact a, Fact b, DataType at, DataType bt, DataType result) {
    if (a.kind == BOTTOM || b.kind == BOTTOM) return bottom();
    if (a.kind != CONSTANT || b.kind != CONSTANT) return unknown();
    if (op == TOKEN_AMP_AMP) return literal(truth(a,at) && truth(b,bt));
    if (op == TOKEN_PIPE_PIPE) return literal(truth(a,at) || truth(b,bt));
    if (floating(at) || floating(bt)) {
        DataType operation=floating(result) ? result : at == TYPE_DOUBLE || bt == TYPE_DOUBLE ? TYPE_DOUBLE : TYPE_FLOAT;
        double x=number(convert(a,at,operation,0),operation), y=number(convert(b,bt,operation,0),operation);
        if (!finite_number(x) || !finite_number(y)) return unknown();
        if (op == TOKEN_EQUAL_EQUAL) return literal(x == y);
        if (op == TOKEN_BANG_EQUAL) return literal(x != y);
        if (op == TOKEN_LESS) return literal(x < y);
        if (op == TOKEN_LESS_EQUAL) return literal(x <= y);
        if (op == TOKEN_GREATER) return literal(x > y);
        if (op == TOKEN_GREATER_EQUAL) return literal(x >= y);
        if (op == TOKEN_SLASH && y == 0) return unknown();
        double value;
        if (operation == TYPE_FLOAT) {
            float left=(float)x, right=(float)y, single;
            if (op == TOKEN_PLUS) single=left+right;
            else if (op == TOKEN_MINUS) single=left-right;
            else if (op == TOKEN_STAR) single=left*right;
            else if (op == TOKEN_SLASH) single=left/right;
            else return unknown();
            value=single;
        } else {
            if (op == TOKEN_PLUS) value=x+y;
            else if (op == TOKEN_MINUS) value=x-y;
            else if (op == TOKEN_STAR) value=x*y;
            else if (op == TOKEN_SLASH) value=x/y;
            else return unknown();
        }
        return finite_number(value) ? real(value,result) : unknown();
    }
    DataType operation=data_type_promoted_integer(at,bt);
    if (data_type_fixed_integer(operation)) {
        a=convert(a,at,operation,0); b=convert(b,bt,operation,0);
        if (data_type_unsigned(operation)) {
            switch (op) {
                case TOKEN_SLASH: case TOKEN_PERCENT:
                    if (!b.bits) return unknown();
                    return literal(op == TOKEN_SLASH ? a.bits/b.bits : a.bits%b.bits);
                case TOKEN_LESS: return literal(a.bits < b.bits);
                case TOKEN_LESS_EQUAL: return literal(a.bits <= b.bits);
                case TOKEN_GREATER: return literal(a.bits > b.bits);
                case TOKEN_GREATER_EQUAL: return literal(a.bits >= b.bits);
                default: break;
            }
        }
    }
    int64_t x=signed_bits(a.bits), y=signed_bits(b.bits);
    switch (op) {
        case TOKEN_PLUS: return convert(literal(a.bits+b.bits),result,result,0);
        case TOKEN_MINUS: return convert(literal(a.bits-b.bits),result,result,0);
        case TOKEN_STAR: return convert(literal(a.bits*b.bits),result,result,0);
        case TOKEN_SLASH: case TOKEN_PERCENT:
            if (!y || (x == INT64_MIN && y == -1)) return unknown();
            return convert(literal((uint64_t)(op == TOKEN_SLASH ? x/y : x%y)),result,result,0);
        case TOKEN_EQUAL_EQUAL: return literal(x == y);
        case TOKEN_BANG_EQUAL: return literal(x != y);
        case TOKEN_LESS: return literal(x < y);
        case TOKEN_LESS_EQUAL: return literal(x <= y);
        case TOKEN_GREATER: return literal(x > y);
        case TOKEN_GREATER_EQUAL: return literal(x >= y);
        default: return unknown();
    }
}
static void free_graph(Graph *g) {
    free(g->blocks); free(g->edges); free(g->owner); free(g->labels); free(g->definitions); memset(g,0,sizeof(*g));
}
static int graph(Graph *g, const IrFunction *f) {
    size_t n=f->instruction_count;
    if (!n) return 1;
    g->blocks=calloc(n,sizeof(*g->blocks)); g->edges=calloc(n,2*sizeof(*g->edges));
    g->owner=malloc(n*sizeof(*g->owner));
    g->labels=malloc(f->next_label*sizeof(*g->labels)); g->definitions=malloc(f->next_value*sizeof(*g->definitions));
    if (!g->blocks || !g->edges || !g->owner || (f->next_label && !g->labels) || (f->next_value && !g->definitions)) return 0;
    for (size_t i=0;i<f->next_label;++i) g->labels[i]=IR_VALUE_NONE;
    for (size_t i=0;i<f->next_value;++i) g->definitions[i]=IR_VALUE_NONE;
    for (size_t i=0;i<n;++i) {
        const IrInstruction *in=&f->instructions[i];
        if (!i || in->opcode == IR_OP_LABEL) {
            if (g->count) g->blocks[g->count-1].end=i;
            Block *b=&g->blocks[g->count++]; b->begin=i; b->predecessor=b->successor[0]=b->successor[1]=IR_VALUE_NONE;
        }
        g->owner[i]=g->count-1;
        if (in->opcode == IR_OP_LABEL) g->labels[in->target_a]=g->count-1;
        if (in->result != IR_VALUE_NONE) g->definitions[in->result]=i;
    }
    g->blocks[g->count-1].end=n;
    size_t edges=0;
    for (size_t i=0;i<g->count;++i) {
        Block *b=&g->blocks[i]; const IrInstruction *last=&f->instructions[b->end-1];
        if (last->opcode == IR_OP_BRANCH || last->opcode == IR_OP_JUMP) b->successor[0]=g->labels[last->target_a];
        else if (last->opcode != IR_OP_RETURN && last->opcode != IR_OP_TRAP && i+1<g->count) b->successor[0]=i+1;
        if (last->opcode == IR_OP_BRANCH && last->target_b != last->target_a) b->successor[1]=g->labels[last->target_b];
        for (size_t s=0;s<2;++s) if (b->successor[s] != IR_VALUE_NONE) {
            Block *to=&g->blocks[b->successor[s]];
            g->edges[edges]=(Edge){i,to->predecessor}; to->predecessor=edges++;
        }
    }
    g->blocks[0].reachable=1;
    int changed;
    do {
        changed=0;
        for (size_t i=0;i<g->count;++i) if (g->blocks[i].reachable)
            for (size_t s=0;s<2;++s) {
                size_t to=g->blocks[i].successor[s];
                if (to != IR_VALUE_NONE && !g->blocks[to].reachable) { g->blocks[to].reachable=1; changed=1; }
            }
    } while (changed);
    return 1;
}
static const IrInstruction *definition(Pass *p,size_t v) {
    return v == IR_VALUE_NONE || p->graph.definitions[v] == IR_VALUE_NONE ? NULL :
        &p->function->instructions[p->graph.definitions[v]];
}
static void protect(Pass *p,size_t v) {
    if (v == IR_VALUE_NONE || p->protected_values[v]) return;
    p->protected_values[v]=1;
    const IrInstruction *in=definition(p,v);
    if (in) { protect(p,in->operand_a); protect(p,in->operand_b); }
}
static size_t local(Pass *p,const IrInstruction *in) {
    return in && in->opcode == IR_OP_LOAD && in->symbol_id < p->module->semantics->symbol_count ?
        p->local_index[in->symbol_id] : IR_VALUE_NONE;
}
static void value_use(Pass *p,size_t v) { if (v != IR_VALUE_NONE) p->address_only[v]=0; }
static int initialize(Pass *p) {
    IrFunction *f=p->function; size_t symbols=p->module->semantics->symbol_count, values=f->next_value, n=f->instruction_count;
    if (symbols > SIZE_MAX/sizeof(Local) || symbols > SIZE_MAX/sizeof(size_t) ||
        values > SIZE_MAX/sizeof(Fact) || values > SIZE_MAX/sizeof(size_t)) return 0;
    if (!graph(&p->graph,f)) return 0;
    p->locals=calloc(symbols,sizeof(*p->locals)); p->local_index=malloc(symbols*sizeof(*p->local_index));
    p->protected_values=calloc(values,1); p->address_only=malloc(values); p->remove=calloc(n,1);
    p->values=malloc(values*sizeof(*p->values)); p->aliases=malloc(values*sizeof(*p->aliases));
    if ((symbols && (!p->locals || !p->local_index)) || (values && (!p->protected_values || !p->address_only || !p->values || !p->aliases)) || (n && !p->remove)) return 0;
    for (size_t i=0;i<symbols;++i) p->local_index[i]=IR_VALUE_NONE;
    for (size_t i=0;i<values;++i) { p->values[i]=bottom(); p->aliases[i]=i; p->address_only[i]=1; }
    for (size_t i=0;i<f->parameter_count;++i) if (!f->parameters[i].is_receiver && scalar(p->module,f->parameters[i].type_id)) {
        size_t symbol=f->parameters[i].symbol_id;
        p->local_index[symbol]=p->local_count;
        p->locals[p->local_count++]=(Local){symbol,IR_VALUE_NONE,f->parameters[i].type_id};
    }
    for (size_t i=0;i<n;++i) {
        const IrInstruction *in=&f->instructions[i];
        if (in->opcode == IR_OP_DECLARE && scalar(p->module,in->type_id)) {
            p->local_index[in->symbol_id]=p->local_count;
            p->locals[p->local_count++]=(Local){in->symbol_id,i,in->type_id};
        }
        if (in->opcode == IR_OP_STORE) protect(p,in->operand_a);
        if (in->opcode == IR_OP_UNARY && in->operator_type == TOKEN_AMPERSAND) protect(p,in->operand_b);
        if (in->opcode == IR_OP_MEMBER || in->opcode == IR_OP_SLICE_LENGTH || in->opcode == IR_OP_SLICE_DATA || in->opcode == IR_OP_SLICE) protect(p,in->operand_a);
        if ((in->opcode == IR_OP_CALL || in->opcode == IR_OP_ENUM_CONSTRUCT)) {
            protect(p,in->operand_a);
            if (in->symbol_id != AST_SYMBOL_NONE)
                for (size_t c=0;c<p->module->function_count;++c) if (p->module->functions[c].symbol_id == in->symbol_id &&
                    p->module->functions[c].parameter_count && p->module->functions[c].parameters[0].is_receiver)
                    protect(p,f->arguments[in->first_argument]);
        }
        if (in->opcode != IR_OP_STORE) value_use(p,in->operand_a);
        value_use(p,in->operand_b);
        for (size_t a=0;a<in->argument_count;++a) value_use(p,f->arguments[in->first_argument+a]);
    }
    /* Taking a local's address makes interprocedural aliasing unknown. */
    for (size_t i=0;i<n;++i) {
        const IrInstruction *in=&f->instructions[i];
        if (in->opcode == IR_OP_UNARY && in->operator_type == TOKEN_AMPERSAND) {
            const IrInstruction *to=definition(p,in->operand_b);
            if (to && to->opcode == IR_OP_LOAD && to->symbol_id < symbols) p->local_index[to->symbol_id]=IR_VALUE_NONE;
        }
    }
    size_t rows=p->graph.count;
    if (p->local_count && rows > SIZE_MAX/p->local_count/sizeof(Fact)) return 0;
    size_t cells=rows*p->local_count;
    p->incoming=malloc(cells*sizeof(Fact)); p->outgoing=malloc(cells*sizeof(Fact)); p->scratch=malloc(p->local_count*sizeof(Fact));
    if ((cells && (!p->incoming || !p->outgoing)) || (p->local_count && !p->scratch)) return 0;
    for (size_t i=0;i<cells;++i) p->incoming[i]=p->outgoing[i]=bottom();
    return 1;
}
static void release(Pass *p) {
    free_graph(&p->graph); free(p->locals); free(p->local_index); free(p->protected_values);
    free(p->address_only); free(p->remove); free(p->values); free(p->aliases);
    free(p->incoming); free(p->outgoing); free(p->scratch);
}
static Fact constant(Pass *p,const IrInstruction *in) {
    if (!numeric(p->module,in->type_id)) return unknown();
    if (in->has_immediate) return literal(in->immediate);
    const AstToken *token=ast_program_token(p->function->source_program,in->auxiliary_token);
    if (!token) return unknown();
    if (floating(in->type)) return real(strtod(token->lexeme,NULL),in->type);
    if (token->type == TOKEN_CHAR_LITERAL) return literal((unsigned char)token->lexeme[0]);
    if (!strcmp(token->lexeme,"true")) return literal(1);
    if (!strcmp(token->lexeme,"false")) return literal(0);
    return literal((uint64_t)strtoull(token->lexeme,NULL,10));
}
static Fact operand(Pass *p,size_t v) { return v == IR_VALUE_NONE ? unknown() : p->values[v]; }
static Fact expression(Pass *p,const IrInstruction *in) {
    Fact a=operand(p,in->operand_a), b=operand(p,in->operand_b);
    const IrInstruction *ad=definition(p,in->operand_a), *bd=definition(p,in->operand_b);
    if (in->opcode == IR_OP_CONSTANT) return constant(p,in);
    if (in->opcode == IR_OP_LOAD) {
        const char *name=ast_program_lexeme(p->function->source_program,in->auxiliary_token);
        if (!strcmp(name,"true")) return literal(1);
        if (!strcmp(name,"false")) return literal(0);
    }
    if (in->opcode == IR_OP_BINARY && numeric(p->module,in->type_id) && ad && bd &&
        numeric(p->module,ad->type_id) && numeric(p->module,bd->type_id))
        return fold_binary(in->operator_type,a,b,ad->type,bd->type,in->type);
    if (in->opcode == IR_OP_UNARY && b.kind == CONSTANT && bd) {
        if (in->operator_type == TOKEN_BANG && numeric(p->module,bd->type_id)) return literal(!truth(b,bd->type));
        if (in->operator_type == TOKEN_MINUS && numeric(p->module,in->type_id))
            return convert(literal(floating(in->type) ? b.bits ^ (in->type == TYPE_FLOAT ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000)) : 0-b.bits),in->type,in->type,0);
    }
    if (in->opcode == IR_OP_CAST && ad && scalar(p->module,in->type_id) &&
        p->module->types[in->type_id].kind != IR_TYPE_POINTER && p->module->types[ad->type_id].kind != IR_TYPE_POINTER)
        return a.kind == BOTTOM ? bottom() : convert(a,ad->type,in->type,1);
    if (in->opcode == IR_OP_PHI) return meet(a,b);
    return unknown();
}
/* Local storage currently uses full virtual slots for declarations/loads and
   typed-width writes for stores. Preserve upper bytes on a partial write. */
static Fact stored(Pass *p,const IrInstruction *in,size_t l,Fact old) {
    const IrInstruction *rhs=definition(p,in->operand_b);
    Fact value=operand(p,in->operand_b);
    int full=p->module->types[in->type_id].kind == IR_TYPE_POINTER || data_type_bytes(in->type) == 8;
    if (in->operator_type != TOKEN_EQUAL) {
        if (old.kind != CONSTANT) return unknown();
        Fact previous=old;
        if (data_type_fixed_integer(in->type)) previous=literal(data_type_normalize_integer(old.bits,in->type));
        else if (in->type == TYPE_INT) previous=literal((uint64_t)(int64_t)(int32_t)old.bits);
        else if (in->type == TYPE_CHAR) previous=literal((uint64_t)(int64_t)(int8_t)old.bits);
        else if (in->type == TYPE_BYTE || in->type == TYPE_BIT) previous=literal(old.bits&255);
        else if (in->type == TYPE_FLOAT) previous=literal(old.bits&UINT32_MAX);
        TokenType operation=in->operator_type == TOKEN_PLUS_EQUAL || in->operator_type == TOKEN_PLUS_PLUS ? TOKEN_PLUS :
            in->operator_type == TOKEN_MINUS_EQUAL || in->operator_type == TOKEN_MINUS_MINUS ? TOKEN_MINUS :
            in->operator_type == TOKEN_STAR_EQUAL ? TOKEN_STAR : TOKEN_SLASH;
        if (in->operator_type == TOKEN_PLUS_PLUS || in->operator_type == TOKEN_MINUS_MINUS) {
            value=floating(in->type) ? real(1,in->type) : literal(1);
        } else if (rhs) value=convert(value,rhs->type,in->type,0);
        value=fold_binary(operation,previous,value,in->type,in->type,in->type);
    } else if (rhs) {
        if (value.kind == CONSTANT) value=convert(value,rhs->type,in->type,0);
        else if (rhs->type_id == p->locals[l].type) value=reference(rhs->result);
        else value=unknown();
    }
    if (full) return value;
    if (old.kind != CONSTANT || value.kind != CONSTANT) return unknown();
    uint64_t mask=(UINT64_C(1) << (data_type_bytes(in->type)*8))-1;
    return literal((old.bits & ~mask) | (value.bits & mask));
}
static int analyze(Pass *p) {
    IrFunction *f=p->function; Graph *g=&p->graph; size_t locals=p->local_count;
    int changed;
    do {
        changed=0;
        for (size_t block=0;block<g->count;++block) if (g->blocks[block].reachable) {
            for (size_t l=0;l<locals;++l) p->scratch[l]=block == 0 ? unknown() : bottom();
            if (block) for (size_t e=g->blocks[block].predecessor;e != IR_VALUE_NONE;e=g->edges[e].next)
                if (g->blocks[g->edges[e].block].reachable)
                    for (size_t l=0;l<locals;++l) p->scratch[l]=meet(p->scratch[l],p->outgoing[g->edges[e].block*locals+l]);
            for (size_t l=0;l<locals;++l) p->incoming[block*locals+l]=p->scratch[l];
            for (size_t i=g->blocks[block].begin;i<g->blocks[block].end;++i) {
                const IrInstruction *in=&f->instructions[i]; Fact fact=expression(p,in);
                size_t l=local(p,in);
                if (l != IR_VALUE_NONE) {
                    fact=p->scratch[l];
                    if (fact.kind == UNKNOWN) { fact=reference(in->result); p->scratch[l]=fact; }
                    if (fact.kind == VALUE && fact.value != in->result && p->values[fact.value].kind == CONSTANT) fact=p->values[fact.value];
                }
                if (in->opcode == IR_OP_DECLARE && in->symbol_id < p->module->semantics->symbol_count) {
                    l=p->local_index[in->symbol_id];
                    if (l != IR_VALUE_NONE) {
                        Fact initial=operand(p,in->operand_a);
                        const IrInstruction *source=definition(p,in->operand_a);
                        if (!source) initial=literal(0);
                        else if (initial.kind == CONSTANT) initial=convert(initial,source->type,in->type,0);
                        else if (source->type_id == in->type_id) initial=reference(source->result);
                        else initial=unknown();
                        p->scratch[l]=initial;
                    }
                }
                if (in->opcode == IR_OP_STORE) {
                    const IrInstruction *target=definition(p,in->operand_a);
                    l=local(p,target);
                    if (l != IR_VALUE_NONE) p->scratch[l]=stored(p,in,l,p->scratch[l]);
                    else if (!target || target->opcode != IR_OP_LOAD)
                        for (size_t q=0;q<locals;++q) p->scratch[q]=unknown();
                }
                if (in->result != IR_VALUE_NONE) {
                    if (data_type_fixed_integer(in->type) && numeric(p->module,in->type_id) && fact.kind == CONSTANT)
                        fact=convert(fact,in->type,in->type,0);
                    if (fact.kind == UNKNOWN) fact=reference(in->result);
                    if (!equal(fact,p->values[in->result])) { p->values[in->result]=fact; changed=1; }
                }
            }
            for (size_t l=0;l<locals;++l) if (!equal(p->scratch[l],p->outgoing[block*locals+l])) {
                p->outgoing[block*locals+l]=p->scratch[l]; changed=1;
            }
        }
    } while (changed);
    return 1;
}
static size_t resolve(Pass *p,size_t v) {
    if (v == IR_VALUE_NONE) return v;
    size_t root=v;
    while (p->aliases[root] != root) root=p->aliases[root];
    while (p->aliases[v] != v) { size_t next=p->aliases[v]; p->aliases[v]=root; v=next; }
    return root;
}
static void make_constant(IrInstruction *in,Fact value) {
    in->opcode=IR_OP_CONSTANT; in->has_immediate=1; in->immediate=value.bits;
    in->operand_a=in->operand_b=in->target_a=in->target_b=IR_VALUE_NONE;
    in->auxiliary_token=AST_TOKEN_NONE; in->symbol_id=AST_SYMBOL_NONE;
    in->argument_count=0; in->first_argument=IR_VALUE_NONE; in->operator_type=TOKEN_ERROR;
}
static size_t identity(Pass *p,const IrInstruction *in) {
    const IrInstruction *a=definition(p,in->operand_a), *b=definition(p,in->operand_b);
    if (in->opcode == IR_OP_CAST && a && a->type_id == in->type_id &&
        (in->type == TYPE_INT || floating(in->type))) return in->operand_a;
    if (in->opcode == IR_OP_UNARY && in->operator_type == TOKEN_MINUS && b &&
        b->opcode == IR_OP_UNARY && b->operator_type == TOKEN_MINUS) return b->operand_b;
    if (in->opcode == IR_OP_UNARY && in->operator_type == TOKEN_AMPERSAND && b &&
        b->opcode == IR_OP_UNARY && b->operator_type == TOKEN_STAR) return b->operand_b;
    if (in->opcode != IR_OP_BINARY || !numeric(p->module,in->type_id) || floating(in->type) ||
        !a || !b || floating(a->type) || floating(b->type)) return IR_VALUE_NONE;
    Fact af=operand(p,in->operand_a), bf=operand(p,in->operand_b);
    if (bf.kind == CONSTANT && ((bf.bits == 0 && (in->operator_type == TOKEN_PLUS || in->operator_type == TOKEN_MINUS)) ||
        (bf.bits == 1 && (in->operator_type == TOKEN_STAR || in->operator_type == TOKEN_SLASH)))) return in->operand_a;
    if (af.kind == CONSTANT && ((af.bits == 0 && in->operator_type == TOKEN_PLUS) ||
        (af.bits == 1 && in->operator_type == TOKEN_STAR))) return in->operand_b;
    return IR_VALUE_NONE;
}
static int pure(Pass *p,const IrInstruction *in) {
    if (in->opcode == IR_OP_LOAD) {
        if (in->symbol_id == AST_SYMBOL_NONE) return 1;
        SemanticSymbolKind kind=p->module->semantics->symbols[in->symbol_id].kind;
        return kind != SEMANTIC_SYMBOL_FIELD;
    }
    if (in->opcode == IR_OP_CONSTANT || in->opcode == IR_OP_CAST ||
        in->opcode == IR_OP_PHI || in->opcode == IR_OP_SLICE_LENGTH) return 1;
    if (in->opcode == IR_OP_UNARY) return in->operator_type != TOKEN_STAR;
    if (in->opcode == IR_OP_BINARY) return in->type != TYPE_STRING &&
        in->operator_type != TOKEN_SLASH && in->operator_type != TOKEN_PERCENT;
    return 0;
}
static int memory_effect(const IrInstruction *in) {
    return in->opcode == IR_OP_STORE || in->opcode == IR_OP_DECLARE || (in->opcode == IR_OP_CALL || in->opcode == IR_OP_ENUM_CONSTRUCT) ||
        in->opcode == IR_OP_FREE || in->opcode == IR_OP_ALLOC ||
        (in->opcode == IR_OP_BINARY && in->type == TYPE_STRING);
}
static int same_read(const IrInstruction *a,const IrInstruction *b) {
    return a->opcode == b->opcode && a->type_id == b->type_id && a->operand_a == b->operand_a &&
        a->operand_b == b->operand_b && a->symbol_id == b->symbol_id && a->operator_type == b->operator_type;
}
static int rewrite(Pass *p) {
    IrFunction *f=p->function; int changed=0;
    for (size_t i=0;i<f->instruction_count;++i) {
        IrInstruction *in=&f->instructions[i];
        if (!p->graph.blocks[p->graph.owner[i]].reachable) { p->remove[i]=1; changed=1; continue; }
        if (in->opcode == IR_OP_BRANCH) {
            Fact value=operand(p,in->operand_a);
            if (value.kind == CONSTANT || in->target_a == in->target_b) {
                if (value.kind == CONSTANT && !truth(value,definition(p,in->operand_a)->type)) in->target_a=in->target_b;
                in->opcode=IR_OP_JUMP; in->operand_a=IR_VALUE_NONE; in->target_b=IR_VALUE_NONE;
                p->stats->branches_folded++; changed=1;
            }
        }
        if (in->opcode == IR_OP_PHI) {
            int a=p->graph.blocks[p->graph.labels[in->target_a]].reachable;
            int b=p->graph.blocks[p->graph.labels[in->target_b]].reachable;
            if (!a || !b) {
                p->aliases[in->result]=a ? in->operand_a : in->operand_b;
                p->remove[i]=1; changed=1; p->stats->copies_propagated++; continue;
            }
        }
        if (in->result == IR_VALUE_NONE || p->protected_values[in->result]) continue;
        Fact fact=p->values[in->result];
        if (fact.kind == CONSTANT && in->opcode != IR_OP_CONSTANT && numeric(p->module,in->type_id)) {
            if (in->opcode == IR_OP_LOAD) p->stats->constants_propagated++; else p->stats->constants_folded++;
            make_constant(in,fact); changed=1; continue;
        }
        size_t copy=fact.kind == VALUE && fact.value != in->result ? fact.value : identity(p,in);
        const IrInstruction *source=definition(p,copy);
        if (source && source->type_id == in->type_id) {
            p->aliases[in->result]=resolve(p,copy); p->remove[i]=1; changed=1;
            if (in->opcode == IR_OP_UNARY && in->operator_type == TOKEN_AMPERSAND) p->stats->addresses_simplified++;
            else p->stats->copies_propagated++;
        }
        /* Integer identities that produce a constant, without dropping effects
           of evaluating either operand. Floating identities are deliberately excluded. */
        if (!p->remove[i] && in->opcode == IR_OP_BINARY && numeric(p->module,in->type_id) &&
            !floating(in->type) && !floating(definition(p,in->operand_a)->type) && !floating(definition(p,in->operand_b)->type)) {
            size_t a=resolve(p,in->operand_a), b=resolve(p,in->operand_b);
            Fact af=operand(p,a), bf=operand(p,b);
            int zero=(in->operator_type == TOKEN_MINUS && a == b) ||
                (in->operator_type == TOKEN_STAR && ((af.kind == CONSTANT && !af.bits) || (bf.kind == CONSTANT && !bf.bits)));
            if (zero) { make_constant(in,literal(0)); changed=1; p->stats->constants_folded++; }
        }
    }
    /* Repeated address/read operations within a block and memory epoch. */
    for (size_t block=0;block<p->graph.count;++block) {
        size_t begin=p->graph.blocks[block].begin;
        if (!p->graph.blocks[block].reachable) { p->stats->blocks_removed++; continue; }
        for (size_t i=begin;i<p->graph.blocks[block].end;++i) {
            IrInstruction *in=&f->instructions[i];
            if (p->remove[i]) continue;
            if (memory_effect(in)) { begin=i+1; continue; }
            if (in->result == IR_VALUE_NONE || p->protected_values[in->result]) continue;
            if (in->opcode == IR_OP_UNARY && in->operator_type == TOKEN_STAR) {
                const IrInstruction *address=definition(p,in->operand_b);
                const IrInstruction *to=address && address->opcode == IR_OP_UNARY && address->operator_type == TOKEN_AMPERSAND ?
                    definition(p,address->operand_b) : NULL;
                if (to && to->opcode == IR_OP_LOAD && scalar(p->module,to->type_id) &&
                    (to->type == TYPE_DOUBLE || to->type == TYPE_STRING || p->module->types[to->type_id].kind == IR_TYPE_POINTER)) {
                    in->opcode=IR_OP_LOAD; in->symbol_id=to->symbol_id; in->auxiliary_token=to->auxiliary_token;
                    in->operand_b=IR_VALUE_NONE; p->stats->addresses_simplified++; changed=1;
                }
            }
            if (in->opcode != IR_OP_INDEX && in->opcode != IR_OP_MEMBER &&
                !(in->opcode == IR_OP_UNARY && (in->operator_type == TOKEN_STAR || in->operator_type == TOKEN_AMPERSAND))) continue;
            for (size_t j=begin;j<i;++j) if (!p->remove[j] && same_read(in,&f->instructions[j])) {
                p->aliases[in->result]=resolve(p,f->instructions[j].result); p->remove[i]=1;
                p->stats->addresses_simplified++; changed=1; break;
            }
        }
    }
    for (size_t i=0;i<f->instruction_count;++i) if (!p->remove[i]) {
        IrInstruction *in=&f->instructions[i];
        in->operand_a=resolve(p,in->operand_a); in->operand_b=resolve(p,in->operand_b);
        for (size_t a=0;a<in->argument_count;++a) {
            size_t *v=&f->arguments[in->first_argument+a]; *v=resolve(p,*v);
        }
    }
    return changed;
}
static void mark(Pass *p,size_t v,unsigned char *live) {
    if (v == IR_VALUE_NONE || live[v]) return;
    live[v]=1; const IrInstruction *in=definition(p,v);
    if (!in) return;
    mark(p,in->operand_a,live); mark(p,in->operand_b,live);
    for (size_t a=0;a<in->argument_count;++a) mark(p,p->function->arguments[in->first_argument+a],live);
}
static int dead_values(Pass *p) {
    IrFunction *f=p->function; unsigned char *live=calloc(f->next_value,1);
    if (f->next_value && !live) return -1;
    for (size_t i=0;i<f->instruction_count;++i) if (!p->remove[i] && !pure(p,&f->instructions[i])) {
        IrInstruction *in=&f->instructions[i];
        mark(p,in->result,live); mark(p,in->operand_a,live); mark(p,in->operand_b,live);
        for (size_t a=0;a<in->argument_count;++a) mark(p,f->arguments[in->first_argument+a],live);
    }
    int changed=0;
    for (size_t i=0;i<f->instruction_count;++i) {
        IrInstruction *in=&f->instructions[i];
        if (!p->remove[i] && in->result != IR_VALUE_NONE && !live[in->result] && pure(p,in)) {
            p->remove[i]=1; changed=1; p->stats->dead_instructions++;
        }
    }
    free(live); return changed;
}
static void transfer_live(Pass *p,size_t block,unsigned char *live,int remove) {
    IrFunction *f=p->function;
    for (size_t i=p->graph.blocks[block].end;i>p->graph.blocks[block].begin;) {
        --i; if (p->remove[i]) continue;
        const IrInstruction *in=&f->instructions[i]; size_t l=local(p,in);
        if (l != IR_VALUE_NONE && !p->address_only[in->result]) live[l]=1;
        if (in->opcode == IR_OP_STORE) {
            l=local(p,definition(p,in->operand_a));
            if (l == IR_VALUE_NONE) continue;
            int trap=in->operator_type == TOKEN_SLASH_EQUAL && !floating(in->type);
            if (!live[l] && !trap && remove) { p->remove[i]=1; p->stats->dead_stores++; continue; }
            if (in->operator_type == TOKEN_EQUAL) live[l]=0;
            else if (live[l] || trap) live[l]=1;
        }
        if (in->opcode == IR_OP_DECLARE && in->symbol_id < p->module->semantics->symbol_count) {
            l=p->local_index[in->symbol_id]; if (l != IR_VALUE_NONE) live[l]=0;
        }
    }
}
static int dead_stores(Pass *p) {
    size_t locals=p->local_count, rows=p->graph.count, cells=locals*rows;
    unsigned char *input=calloc(cells,1), *output=calloc(cells,1), *scratch=calloc(locals,1);
    if ((cells && (!input || !output)) || (locals && !scratch)) { free(input); free(output); free(scratch); return -1; }
    /* Refresh value-vs-address uses after copy propagation and DCE. */
    for (size_t v=0;v<p->function->next_value;++v) p->address_only[v]=1;
    for (size_t i=0;i<p->function->instruction_count;++i) if (!p->remove[i]) {
        const IrInstruction *in=&p->function->instructions[i];
        if (in->opcode != IR_OP_STORE) value_use(p,in->operand_a);
        value_use(p,in->operand_b);
        for (size_t a=0;a<in->argument_count;++a) value_use(p,p->function->arguments[in->first_argument+a]);
    }
    int changed;
    do {
        changed=0;
        for (size_t b=rows;b>0;) {
            --b; if (!p->graph.blocks[b].reachable) continue;
            for (size_t l=0;l<locals;++l) scratch[l]=0;
            for (size_t s=0;s<2;++s) {
                size_t to=p->graph.blocks[b].successor[s];
                if (to != IR_VALUE_NONE) for (size_t l=0;l<locals;++l) scratch[l]|=input[to*locals+l];
            }
            if (locals) memcpy(output+b*locals,scratch,locals);
            transfer_live(p,b,scratch,0);
            for (size_t l=0;l<locals;++l) if (scratch[l] != input[b*locals+l]) { input[b*locals+l]=scratch[l]; changed=1; }
        }
    } while (changed);
    size_t before=p->stats->dead_stores;
    for (size_t b=0;b<rows;++b) if (p->graph.blocks[b].reachable) {
        if (locals) memcpy(scratch,output+b*locals,locals);
        transfer_live(p,b,scratch,1);
    }
    free(input); free(output); free(scratch);
    return p->stats->dead_stores != before;
}
static int compact(Pass *p) {
    IrFunction *f=p->function;
    /* Drop declarations only after every reference to their storage disappeared. */
    for (size_t l=0;l<p->local_count;++l) {
        size_t d=p->locals[l].declaration;
        if (d == IR_VALUE_NONE) continue;
        if (p->local_index[p->locals[l].symbol] == IR_VALUE_NONE) continue;
        int used=0;
        for (size_t i=0;i<f->instruction_count;++i)
            if (!p->remove[i] && f->instructions[i].opcode == IR_OP_LOAD && f->instructions[i].symbol_id == p->locals[l].symbol) { used=1; break; }
        if (!used && !p->remove[d]) { p->remove[d]=1; p->stats->dead_instructions++; }
    }
    size_t n=0;
    for (size_t i=0;i<f->instruction_count;++i) if (!p->remove[i]) f->instructions[n++]=f->instructions[i];
    int changed=n != f->instruction_count; f->instruction_count=n;
    /* Remaining PHIs stay at the front even if preceding PHIs became constants. */
    for (size_t i=0;i<n;++i) if (f->instructions[i].opcode == IR_OP_LABEL) {
        size_t end=i+1;
        while (end<n && (f->instructions[end].opcode == IR_OP_PHI || f->instructions[end].opcode == IR_OP_CONSTANT)) ++end;
        size_t insertion=i+1;
        for (size_t j=i+1;j<end;++j) if (f->instructions[j].opcode == IR_OP_PHI) {
            IrInstruction phi=f->instructions[j];
            memmove(&f->instructions[insertion+1],&f->instructions[insertion],(j-insertion)*sizeof(phi));
            f->instructions[insertion++]=phi;
        }
    }
    return changed;
}
static int compact_ids(IrFunction *f) {
    size_t *values=malloc(f->next_value*sizeof(*values)), *labels=malloc(f->next_label*sizeof(*labels));
    size_t *arguments=malloc(f->argument_count*sizeof(*arguments));
    if ((f->next_value && !values) || (f->next_label && !labels) || (f->argument_count && !arguments)) {
        free(values); free(labels); free(arguments); return 0;
    }
    for (size_t v=0;v<f->next_value;++v) values[v]=IR_VALUE_NONE;
    for (size_t l=0;l<f->next_label;++l) labels[l]=IR_VALUE_NONE;
    size_t nv=0,nl=0,na=0;
    for (size_t i=0;i<f->instruction_count;++i) {
        const IrInstruction *in=&f->instructions[i];
        if (in->result != IR_VALUE_NONE) values[in->result]=nv++;
        if (in->opcode == IR_OP_LABEL) labels[in->target_a]=nl++;
    }
    for (size_t i=0;i<f->instruction_count;++i) {
        IrInstruction *in=&f->instructions[i];
        if (in->result != IR_VALUE_NONE) in->result=values[in->result];
        if (in->operand_a != IR_VALUE_NONE) in->operand_a=values[in->operand_a];
        if (in->operand_b != IR_VALUE_NONE) in->operand_b=values[in->operand_b];
        if (in->opcode == IR_OP_LABEL || in->opcode == IR_OP_JUMP || in->opcode == IR_OP_BRANCH || in->opcode == IR_OP_PHI || in->opcode == IR_OP_ENUM_PAYLOAD) {
            in->target_a=labels[in->target_a];
            if (in->target_b != IR_VALUE_NONE) in->target_b=labels[in->target_b];
        }
        if ((in->opcode == IR_OP_CALL || in->opcode == IR_OP_ENUM_CONSTRUCT)) {
            size_t old=in->first_argument; in->first_argument=na;
            for (size_t a=0;a<in->argument_count;++a) arguments[na++]=values[f->arguments[old+a]];
        }
    }
    f->next_value=nv; f->next_label=nl;
    free(f->arguments); f->arguments=arguments; f->argument_count=na;
    /* Allocation may retain spare capacity, but the next lowering cannot access it. */
    f->argument_capacity=na;
    free(values); free(labels); return 1;
}
int ir_optimize_module(IrModule *module,IrOptimizationStats *stats) {
    IrOptimizationStats ignored={0};
    if (!stats) stats=&ignored;
    memset(stats,0,sizeof(*stats));
    if (!ir_verify_module(module)) return 0;
    module->verified=0;
    for (size_t f=0;f<module->function_count;++f) {
        int changed;
        do {
            Pass p={0}; p.module=module; p.function=&module->functions[f]; p.stats=stats;
            if (!initialize(&p)) { release(&p); return 0; }
            analyze(&p); changed=rewrite(&p);
            int dead=dead_values(&p);
            if (dead < 0) { release(&p); return 0; }
            changed|=dead;
            dead=dead_stores(&p);
            if (dead < 0) { release(&p); return 0; }
            changed|=dead;
            changed|=compact(&p);
            release(&p);
        } while (changed);
        if (!compact_ids(&module->functions[f])) return 0;
    }
    module->verified=ir_verify_module(module);
    return module->verified;
}
