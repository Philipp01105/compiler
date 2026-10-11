/* Local/parameter alpha-renaming. Unknown preservation results are conflicts. */
typedef struct {
    size_t start, end;
} RenameEdit;
static int rename_edit_order(const void *a, const void *b) {
    const RenameEdit *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}
static size_t json_offset(const char *text, const JsonValue *point) {
    int64_t line, column;
    return ide_json_integer(ide_json_get(point, "line"), &line) &&
                   ide_json_integer(ide_json_get(point, "character"), &column)
               ? utf16_offset(text, line, column)
               : SIZE_MAX;
}
static JsonValue *rename_graph(const AstProgram *program, const SemanticModel *model) {
    FILE *out = tmpfile();
    if (!out)
        return NULL;
    fputs("{\"occurrences\":[", out);
    int count = 0, incomplete = 0;
    for (size_t i = 0; i <= program->owned_import_count; i++) {
        const AstProgram *p = i ? program->owned_imports[i - 1] : program;
        char *text = read_source(p->source_path);
        if (!text) {
            fclose(out);
            return NULL;
        }
        Graph graph = {.out = out, .model = model, .program = p, .source = text, .count = &count,
                       .incomplete = &incomplete, .function = AST_SYMBOL_NONE, .scope = AST_SYMBOL_NONE,
                       .checked_context = AST_SYMBOL_NONE, .call_arity = -1};
        declaration_uses(&graph, p->root, 0);
        free(text);
    }
    fputs("]}", out);
    char *text = read_stream(out);
    fclose(out);
    JsonValue *result = text ? ide_json_parse(text) : NULL;
    free(text);
    if (incomplete) {
        ide_json_free(result);
        return NULL;
    }
    return result;
}
static size_t rename_mapped_offset(size_t offset, const RenameEdit *edits, size_t count, size_t length) {
    size_t mapped = offset;
    for (size_t i = 0; i < count; i++) {
        if (offset >= edits[i].end)
            mapped = mapped - (edits[i].end - edits[i].start) + length;
        else if (offset > edits[i].start)
            return SIZE_MAX;
    }
    return mapped;
}
static int same_symbol_facts(const SemanticModel *a, const SemanticModel *b, size_t selected, const char *new_name) {
    if (a->symbol_count != b->symbol_count)
        return 0;
    for (size_t i = 0; i < a->symbol_count; i++) {
        const SemanticSymbol *x = &a->symbols[i], *y = &b->symbols[i];
        if (!x->source_program || !y->source_program ||
            strcmp(x->source_program->source_path, y->source_program->source_path) || x->kind != y->kind ||
            x->owner_symbol_id != y->owner_symbol_id || x->scope_depth != y->scope_depth ||
            x->resolved_type != y->resolved_type || x->resolved_borrow_kind != y->resolved_borrow_kind ||
            x->resolved_pointer_depth != y->resolved_pointer_depth ||
            x->resolved_outer_pointer_depth != y->resolved_outer_pointer_depth ||
            x->resolved_named_symbol_id != y->resolved_named_symbol_id ||
            x->resolved_is_array != y->resolved_is_array || x->resolved_is_slice != y->resolved_is_slice ||
            x->type_properties != y->type_properties || x->async_frame_properties != y->async_frame_properties ||
            x->async_graph_properties != y->async_graph_properties ||
            strcmp(ast_program_lexeme(y->source_program, y->name_token),
                   i == selected ? new_name : ast_program_lexeme(x->source_program, x->name_token)))
            return 0;
    }
    return 1;
}
typedef struct {
    AstToken *token;
    const char *lexeme;
} RenameToken;
static void rename_ir_token(const AstProgram *program, size_t index, RenameToken *saved, size_t *count,
                            const char *old) {
    if (index >= program->token_count)
        return;
    AstToken *token = &((AstProgram *)program)->tokens[index];
    if (token->type != TOKEN_IDENTIFIER || strcmp(token->lexeme, old))
        return;
    for (size_t i = 0; i < *count; i++)
        if (saved[i].token == token)
            return;
    saved[(*count)++] = (RenameToken){token, token->lexeme};
    token->lexeme = "__dmm_rename_identity__";
}
static char *rename_ir(const AstProgram *program, const SemanticModel *model, size_t selected) {
    IrModule *module = ir_lower_program(program, model);
    if (!module)
        return NULL;
    const SemanticSymbol *symbol = &model->symbols[selected];
    RenameToken *saved = calloc(symbol->source_program->token_count, sizeof(*saved));
    size_t saved_count = 0;
    if (!saved) {
        ir_module_free(module);
        return NULL;
    }
    const char *old = ast_program_lexeme(symbol->source_program, symbol->name_token);
    for (size_t f = 0; f < module->function_count; f++) {
        IrFunction *function = &module->functions[f];
        for (size_t p = 0; p < function->parameter_count; p++)
            if (function->parameters[p].symbol_id == selected)
                rename_ir_token(function->parameters[p].source_program, function->parameters[p].name_token, saved,
                                &saved_count, old);
        for (size_t i = 0; i < function->instruction_count; i++) {
            IrInstruction *instruction = &function->instructions[i];
            if (instruction->symbol_id == selected)
                rename_ir_token(function->source_program, instruction->auxiliary_token, saved, &saved_count, old);
        }
    }
    FILE *out = tmpfile();
    char *result = NULL;
    if (out) {
        if (ir_dump(out, module)) {
            /* Include instruction attributes omitted by the human-readable dump. */
            for (size_t f = 0; f < module->function_count; f++)
                for (size_t i = 0; i < module->functions[f].instruction_count; i++) {
                    const IrInstruction *in = &module->functions[f].instructions[i];
                    fprintf(out, "facts %zu %zu %llu %d %d %d %zu %zu %d %d %d %d\n", f, i,
                            (unsigned long long)in->runtime_requirements, (int)in->async_operation, in->async_cleanup,
                            in->owns_slice_backing, in->element_count, in->type_name_token, (int)in->operator_type,
                            in->has_immediate, in->uninitialized_storage, in->lifetime_pointer);
                }
            result = read_stream(out);
        }
        fclose(out);
    }
    /* Preserve verifier source spans; omit only unquoted dump position metadata. */
    if (result) {
        size_t read = 0, write = 0;
        int quoted = 0, escaped = 0;
        while (result[read]) {
            if (!quoted && !strncmp(result + read, " span=", 6)) {
                size_t end = read + 6;
                while (isdigit((unsigned char)result[end]) || result[end] == ':' || result[end] == '-')
                    end++;
                if (end > read + 6 && (result[end] == '\n' || result[end] == ' ' || !result[end])) {
                    read = end;
                    continue;
                }
            }
            char c = result[read++];
            result[write++] = c;
            if (quoted && escaped)
                escaped = 0;
            else if (quoted && c == '\\')
                escaped = 1;
            else if (c == '"')
                quoted = !quoted;
        }
        result[write] = 0;
    }
    for (size_t i = 0; i < saved_count; i++)
        saved[i].token->lexeme = saved[i].lexeme;
    free(saved);
    ir_module_free(module);
    return result;
}
static char *ide_rename(const JsonValue *request, const AstProgram *program, const SemanticModel *model,
                        TargetFormat target, const char **conflict) {
    const char *name = ide_json_string(request, "newName");
    char *path = uri_path(ide_json_string(request, "uri"));
    char *source = path ? read_source(path) : NULL;
    JsonValue *before = model && program ? rename_graph(program, model) : NULL;
    JsonValue *after = NULL;
    RenameEdit *edits = NULL;
    char *replacement = NULL, *old_ir = NULL, *new_ir = NULL, *result = NULL;
    AstProgram *proposed = NULL;
    SemanticModel *changed = NULL;
    ErrorHandler *baseline = global_error_handler, *validation = NULL;
    DmmSourceOverlay snapshots[1024] = {0};
    size_t edit_count = 0, selected = AST_SYMBOL_NONE;
    size_t cursor = source ? json_offset(source, ide_json_get(request, "position")) : SIZE_MAX;
    *conflict = "Rename requires a resolved local or parameter in a valid workspace";
    if (!name || !*name || strlen(name) > 200 || !path || !source || !before || cursor == SIZE_MAX ||
        baseline->error_count || baseline->source_snapshot_failed || model->error_count)
        goto cleanup;
    if (!(isalpha((unsigned char)*name) || *name == '_'))
        goto cleanup;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p >= 128 || !(isalnum(*p) || *p == '_'))
            goto cleanup;
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *s = &model->symbols[i];
        if (!s->source_program || path_identity_equal(path, s->source_program->source_path) != 1 ||
            s->name_token >= s->source_program->token_count)
            continue;
        AstSourceSpan span = s->source_program->tokens[s->name_token].span;
        size_t start = utf16_offset(source, span.begin.line - 1, 0), end = utf16_offset(source, span.end.line - 1, 0);
        /* Token columns count code points, not UTF-16 units. */
        if (start == SIZE_MAX || end == SIZE_MAX)
            continue;
        for (int c = 1; c < span.begin.column && source[start] && source[start] != '\n'; c++) {
            start++;
            while (((unsigned char)source[start] & 0xc0) == 0x80)
                start++;
        }
        for (int c = 1; c < span.end.column && source[end] && source[end] != '\n'; c++) {
            end++;
            while (((unsigned char)source[end] & 0xc0) == 0x80)
                end++;
        }
        if (start <= cursor && cursor < end) {
            if (selected != AST_SYMBOL_NONE)
                goto cleanup;
            selected = i;
        }
    }
    const JsonValue *uses = ide_json_get(before, "occurrences");
    for (const JsonValue *o = uses->child; o; o = o->next) {
        char *where = uri_path(ide_json_string(o, "uri"));
        int same = where && path_identity_equal(path, where) == 1;
        free(where);
        if (!same)
            continue;
        const JsonValue *r = ide_json_get(o, "range");
        size_t start = json_offset(source, ide_json_get(r, "start")), end = json_offset(source, ide_json_get(r, "end"));
        int64_t id;
        if (start <= cursor && cursor < end && ide_json_integer(ide_json_get(o, "declaration"), &id) && id >= 0) {
            if (selected != AST_SYMBOL_NONE && selected != (size_t)id)
                goto cleanup;
            selected = (size_t)id;
        }
    }
    if (selected >= model->symbol_count)
        goto cleanup;
    const SemanticSymbol *symbol = &model->symbols[selected];
    if ((symbol->kind != SEMANTIC_SYMBOL_LOCAL && symbol->kind != SEMANTIC_SYMBOL_PARAMETER) ||
        symbol->owner_symbol_id >= model->symbol_count)
        goto cleanup;
    const AstDeclarationNode *owner = model->symbols[symbol->owner_symbol_id].declaration;
    if (!owner || owner->kind != AST_DECL_FUNCTION || owner->generic_origin || owner->generic_parameters ||
        owner->lifetime_parameters || owner->as.function.is_async || owner->is_native)
        goto cleanup;
    /* Generated closure environments can obscure original capture references. */
    for (size_t i = 0; i < model->symbol_count; i++)
        if (model->symbols[i].source_program == symbol->source_program && model->symbols[i].declaration &&
            model->symbols[i].declaration->is_closure_environment)
            goto cleanup;
    const char *old = ast_program_lexeme(symbol->source_program, symbol->name_token);
    size_t old_length = strlen(old), new_length = strlen(name);
    if (!strcmp(old, "self") || !strcmp(old, "Self"))
        goto cleanup;
    /* Build source ranges through the common UTF-16 writer, including the declaration. */
    FILE *declaration = tmpfile();
    if (!declaration)
        goto cleanup;
    range(declaration, source, symbol->source_program->tokens[symbol->name_token].span);
    char *range_text = read_stream(declaration);
    fclose(declaration);
    JsonValue *decl_range = range_text ? ide_json_parse(range_text) : NULL;
    free(range_text);
    if (!decl_range)
        goto cleanup;
    edits = calloc(10001, sizeof(*edits));
    if (!edits) {
        ide_json_free(decl_range);
        goto cleanup;
    }
    edits[edit_count++] = (RenameEdit){json_offset(source, ide_json_get(decl_range, "start")),
                                       json_offset(source, ide_json_get(decl_range, "end"))};
    ide_json_free(decl_range);
    for (const JsonValue *o = uses->child; o; o = o->next) {
        int64_t id;
        if (!ide_json_integer(ide_json_get(o, "declaration"), &id) || id < 0 || (size_t)id != selected)
            continue;
        char *where = uri_path(ide_json_string(o, "uri"));
        int same = where && path_identity_equal(path, where) == 1;
        free(where);
        if (!same || edit_count == 10001)
            goto cleanup;
        const JsonValue *r = ide_json_get(o, "range");
        edits[edit_count++] =
            (RenameEdit){json_offset(source, ide_json_get(r, "start")), json_offset(source, ide_json_get(r, "end"))};
    }
    qsort(edits, edit_count, sizeof(*edits), rename_edit_order);
    size_t unique = 0;
    for (size_t i = 0; i < edit_count; i++) {
        if (edits[i].start == SIZE_MAX || edits[i].end - edits[i].start != old_length ||
            strncmp(source + edits[i].start, old, old_length))
            goto cleanup;
        if (unique && edits[i].start == edits[unique - 1].start && edits[i].end == edits[unique - 1].end)
            continue;
        if (unique && edits[i].start < edits[unique - 1].end)
            goto cleanup;
        edits[unique++] = edits[i];
    }
    edit_count = unique;
    size_t length = strlen(source) - edit_count * old_length + edit_count * new_length;
    if (length > IDE_MAX_INPUT)
        goto cleanup;
    replacement = malloc(length + 1);
    if (!replacement)
        goto cleanup;
    size_t read = 0, write = 0;
    for (size_t i = 0; i < edit_count; i++) {
        size_t prefix = edits[i].start - read;
        memcpy(replacement + write, source + read, prefix);
        write += prefix;
        memcpy(replacement + write, name, new_length);
        write += new_length;
        read = edits[i].end;
    }
    strcpy(replacement + write, source + read);
    validation = error_handler_init();
    if (!validation)
        goto cleanup;
    validation->source_snapshot_enabled = 1;
    for (size_t i = 0; i < baseline->source_snapshot_count; i++) {
        DmmSourceOverlay *entries = realloc(validation->source_snapshot, (i + 1) * sizeof(*entries));
        if (!entries)
            goto cleanup;
        validation->source_snapshot = entries;
        const DmmSourceOverlay *cached = &baseline->source_snapshot[i];
        char *p = malloc(strlen(cached->path) + 1), *text = cached->text ? malloc(cached->length + 1) : NULL;
        if (!p || (cached->text && !text)) {
            free(p);
            free(text);
            goto cleanup;
        }
        strcpy(p, cached->path);
        if (text)
            memcpy(text, cached->text, cached->length + 1);
        entries[i] = (DmmSourceOverlay){p, text, cached->length};
        validation->source_snapshot_count++;
        validation->source_snapshot_bytes += cached->length;
    }
    size_t count = baseline->overlay_count, index = count;
    if (count > 1024)
        goto cleanup;
    memcpy(snapshots, baseline->overlays, count * sizeof(*snapshots));
    for (size_t i = 0; i < count; i++)
        if (path_identity_equal(path, snapshots[i].path) == 1)
            index = i;
    if (index == count && count == 1024)
        goto cleanup;
    snapshots[index] = (DmmSourceOverlay){path, replacement, length};
    if (index == count)
        count++;
    validation->overlays = snapshots;
    validation->overlay_count = count;
    error_handler_set_global(validation);
    FrontendOptions options = {.recover_syntax = 1, .has_target = 1, .target_format = target, .system_packages = 1,
                               .stdlib_root = ide_json_string(request, "stdlibRoot")};
    proposed = frontend_parse_file(program->source_path, &options);
    if (proposed && ast_validate_program(proposed))
        changed = semantic_analyze_target(proposed, target);
    *conflict = "Rename introduces diagnostics or changes semantic bindings/type/ownership facts";
    if (!changed || validation->error_count || validation->source_snapshot_failed || changed->error_count ||
        validation->source_snapshot_count != baseline->source_snapshot_count ||
        !same_symbol_facts(model, changed, selected, name))
        goto cleanup;
    after = rename_graph(proposed, changed);
    if (!after)
        goto cleanup;
    const JsonValue *x = uses->child, *y = ide_json_get(after, "occurrences")->child;
    for (; x && y; x = x->next, y = y->next) {
        const char *a = ide_json_string(x, "uri"), *b = ide_json_string(y, "uri");
        int64_t ai = -1, bi = -1;
        ide_json_integer(ide_json_get(x, "declaration"), &ai);
        ide_json_integer(ide_json_get(y, "declaration"), &bi);
        if (!a || !b || strcmp(a, b) || ai != bi || strcmp(ide_json_string(x, "role"), ide_json_string(y, "role")))
            goto cleanup;
        char *where = uri_path(a);
        int same = where && path_identity_equal(path, where) == 1;
        free(where);
        if (same) {
            const JsonValue *ar = ide_json_get(x, "range"), *br = ide_json_get(y, "range");
            size_t start = json_offset(source, ide_json_get(ar, "start")),
                   end = json_offset(source, ide_json_get(ar, "end"));
            size_t mapped_start = rename_mapped_offset(start, edits, edit_count, new_length);
            size_t mapped_end = rename_mapped_offset(end, edits, edit_count, new_length);
            if (mapped_start != json_offset(replacement, ide_json_get(br, "start")) ||
                mapped_end != json_offset(replacement, ide_json_get(br, "end")))
                goto cleanup;
        }
    }
    if (x || y)
        goto cleanup;
    error_handler_set_global(baseline);
    old_ir = rename_ir(program, model, selected);
    error_handler_set_global(validation);
    new_ir = rename_ir(proposed, changed, selected);
    *conflict = "Rename cannot prove unchanged lowered behavior";
    if (!old_ir || !new_ir || strcmp(old_ir, new_ir) || validation->error_count || baseline->error_count)
        goto cleanup;
    FILE *out = tmpfile();
    if (!out)
        goto cleanup;
    fputs("{\"validated\":true,\"edits\":[", out);
    for (size_t i = 0; i < edit_count; i++) {
        if (i)
            fputc(',', out);
        fputs("{\"uri\":", out);
        uri(out, path);
        fputs(",\"range\":", out);
        range(out, source,
              (AstSourceSpan){source_location(source, edits[i].start), source_location(source, edits[i].end)});
        fputs(",\"oldText\":", out);
        ide_json_quote(out, old);
        fputs(",\"newText\":", out);
        ide_json_quote(out, name);
        fputc('}', out);
    }
    fputs("]}", out);
    result = read_stream(out);
    fclose(out);
    if (result)
        *conflict = NULL;
cleanup:
    error_handler_set_global(baseline);
    semantic_model_free(changed);
    ast_program_free(proposed);
    error_handler_free(validation);
    ide_json_free(before);
    ide_json_free(after);
    free(edits);
    free(replacement);
    free(old_ir);
    free(new_ir);
    free(path);
    free(source);
    return result;
}
