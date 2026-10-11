/* Audit source uses independently of traversal. Numeric IDs remain snapshot-local. */
typedef struct {
    const AstProgram *program;
    AstSourceSpan span;
    const char *role;
    size_t caller;
    unsigned resolved : 1;
    unsigned generated : 1;
    unsigned contextual : 1;
} IdeBindingUse;
typedef struct {
    IdeBindingUse *uses;
    size_t count, capacity;
    int limited;
} IdeBindingAudit;
static int source_symbol_anchor(const SemanticModel *model, size_t id) {
    if (!model || id >= model->symbol_count) return 0;
    const SemanticSymbol *symbol = &model->symbols[id];
    const AstProgram *program = symbol->source_program;
    const AstDeclarationNode *decl = source_declaration(symbol->declaration);
    size_t token = decl ? (decl->is_source_closure ? decl->first_token : decl->name_token) : symbol->name_token;
    return program && token < program->token_count && program->tokens[token].span.begin.line > 0 &&
           program->tokens[token].lexeme && *program->tokens[token].lexeme;
}
static void audit_binding(IdeBindingAudit *audit, const AstProgram *program, AstSourceSpan span,
                          const char *role, size_t caller, int resolved, int generated, int contextual) {
    if (!audit || audit->limited || !strcmp(role, "namespace")) return;
    if (audit->count == audit->capacity) {
        size_t capacity = audit->capacity ? audit->capacity * 2 : 256;
        if (capacity > 1000000) { audit->limited = 1; return; }
        IdeBindingUse *uses = realloc(audit->uses, capacity * sizeof(*uses));
        if (!uses) { audit->limited = 1; return; }
        audit->uses = uses;
        audit->capacity = capacity;
    }
    audit->uses[audit->count++] = (IdeBindingUse){program, span, role, caller,
                                                resolved != 0, generated != 0, contextual != 0};
}
static int binding_use_order(const void *left, const void *right) {
    const IdeBindingUse *a = left, *b = right;
    int order = strcmp(a->program->source_path, b->program->source_path);
    if (order) return order;
    if (a->span.begin.line != b->span.begin.line) return a->span.begin.line < b->span.begin.line ? -1 : 1;
    if (a->span.begin.column != b->span.begin.column) return a->span.begin.column < b->span.begin.column ? -1 : 1;
    if (a->span.end.line != b->span.end.line) return a->span.end.line < b->span.end.line ? -1 : 1;
    if (a->span.end.column != b->span.end.column) return a->span.end.column < b->span.end.column ? -1 : 1;
    return strcmp(a->role, b->role);
}
static void binding_coverage(FILE *out, IdeBindingAudit *audit, const SemanticModel *model, int traversal_complete) {
    if (audit->count) qsort(audit->uses, audit->count, sizeof(*audit->uses), binding_use_order);
    int generated_complete = 1, contextual_complete = 1;
    size_t missing = 0;
    for (size_t i = 0; i < audit->count;) {
        size_t end = i + 1;
        while (end < audit->count && !binding_use_order(&audit->uses[i], &audit->uses[end])) end++;
        int unresolved = 0;
        for (size_t j = i; j < end; j++) {
            const IdeBindingUse *use = &audit->uses[j];
            if (!use->resolved) {
                unresolved = 1;
                if (use->generated) generated_complete = 0;
                if (use->contextual) contextual_complete = 0;
            }
        }
        if (unresolved) missing++;
        i = end;
    }
    int traversed = model && model->source_registry_complete && traversal_complete && !audit->limited;
    fprintf(out, "{\"templateDeclarations\":%s,\"auditComplete\":%s,\"complete\":%s,"
                 "\"generatedBindings\":%s,\"contextDependentBindings\":%s,\"sourceUseCount\":%zu,\"uncovered\":[",
            model && model->source_registry_complete ? "true" : "false", traversed ? "true" : "false",
            traversed && !missing ? "true" : "false", traversed && generated_complete ? "true" : "false",
            traversed && contextual_complete && !missing ? "true" : "false", audit->count);
    const AstProgram *program = NULL;
    char *source = NULL;
    int count = 0;
    for (size_t i = 0; i < audit->count;) {
        size_t end = i + 1;
        while (end < audit->count && !binding_use_order(&audit->uses[i], &audit->uses[end])) end++;
        int unresolved = 0, resolved = 0, checked_resolved = 0, source_resolved = 0;
        for (size_t j = i; j < end; j++) {
            unresolved |= !audit->uses[j].resolved;
            resolved |= audit->uses[j].resolved;
            checked_resolved |= audit->uses[j].resolved && audit->uses[j].contextual;
            source_resolved |= audit->uses[j].resolved && !audit->uses[j].contextual;
        }
        if (unresolved) {
            const IdeBindingUse *use = &audit->uses[i];
            if (program != use->program) {
                free(source);
                program = use->program;
                source = read_source(program->source_path);
            }
            if (count++) fputc(',', out);
            fputs("{\"uri\":", out);
            uri(out, program->source_path);
            fputs(",\"range\":", out);
            range(out, source ? source : "", use->span);
            fputs(",\"role\":", out);
            ide_json_quote(out, use->role);
            if (use->caller != AST_SYMBOL_NONE) fprintf(out, ",\"caller\":%zu", use->caller);
            fputs(",\"reason\":", out);
            ide_json_quote(out, resolved ? checked_resolved && !source_resolved
                                             ? "Source binding is known only in checked contexts"
                                             : "Source binding requires contextual selection of compiler candidates"
                                         : "Source use has no compiler binding with a source anchor");
            fputc('}', out);
        }
        i = end;
    }
    fputs("]}", out);
    free(source);
    free(audit->uses);
}
