#include "ide.h"
#include "errorHandler.h"
#include "frontend.h"
#include "ide_json.h"
#include "ir.h"
#include "path_identity.h"
#include "semantic.h"
#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef DMM_VERSION
#define DMM_VERSION "development"
#endif
#define IDE_MAX_INPUT (32u * 1024u * 1024u)

static char *read_stream(FILE *stream) {
    if (!stream || fseek(stream, 0, SEEK_END))
        return NULL;
    long length = ftell(stream);
    if (length < 0 || (unsigned long)length > IDE_MAX_INPUT || fseek(stream, 0, SEEK_SET))
        return NULL;
    char *text = malloc((size_t)length + 1);
    if (!text)
        return NULL;
    if (fread(text, 1, (size_t)length, stream) != (size_t)length) {
        free(text);
        return NULL;
    }
    text[length] = 0;
    return text;
}
static char *read_source(const char *path) {
    FILE *file = error_handler_open_source(path);
    char *text = read_stream(file);
    if (file)
        fclose(file);
    return text;
}
static int unhex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
}
static char *uri_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7))
        return NULL;
    const char *p = uri + 7;
    if (!strncmp(p, "localhost/", 10))
        p += 9;
    if (*p != '/')
        return NULL;
#ifdef _WIN32
    if (p[1] && p[2] == ':')
        p++;
#endif
    char *decoded = malloc(strlen(p) + 1);
    if (!decoded)
        return NULL;
    size_t n = 0;
    for (; *p; p++) {
        if (*p == '?' || *p == '#') {
            free(decoded);
            return NULL;
        }
        if (*p == '%') {
            if (!p[1] || !p[2] || unhex(p[1]) < 0 || unhex(p[2]) < 0) {
                free(decoded);
                return NULL;
            }
            unsigned c = (unsigned)(unhex(p[1]) * 16 + unhex(p[2]));
            if (!c) {
                free(decoded);
                return NULL;
            }
            decoded[n++] = (char)c;
            p += 2;
        } else
            decoded[n++] = *p;
    }
    decoded[n] = 0;
    char *canonical = path_identity_canonical(decoded);
    free(decoded);
    return canonical;
}
static void uri(FILE *out, const char *path) {
    fputs("\"file://", out);
    if (path[0] != '/')
        fputc('/', out);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || *p == '/' || *p == ':' || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            fputc(*p, out);
        else
            fprintf(out, "%%%02X", *p);
    }
    fputc('"', out);
}
static uint64_t hash(const char *text) {
    uint64_t value = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        value ^= *p;
        value *= UINT64_C(1099511628211);
    }
    return value;
}
typedef struct {
    char *path;
    uint64_t fingerprint;
    int boundary_only;
} IdeDirectory;
typedef struct {
    IdeDirectory *directories;
    size_t count;
} IdeDirectoryInventory;
/* Order-independent sum of FNV-1a(name + NUL + kind), finalized with the entry count. */
static int directory_fingerprint(const char *directory, int boundary_only, uint64_t *fingerprint) {
    DIR *stream = opendir(directory);
    if (!stream)
        return 0;
    uint64_t sum = UINT64_C(14695981039346656037);
    size_t count = 0, visited = 0;
    int ok = 1;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        if (++visited > 10000) {
            ok = 0;
            break;
        }
        const char *name = entry->d_name;
        if (name[0] == '.' || !strcmp(name, "vendor") || (boundary_only && strcmp(name, "dmm.manifest")))
            continue;
        size_t length = strlen(directory) + strlen(name) + 2;
        char *path = malloc(length);
        if (!path) {
            ok = 0;
            break;
        }
        snprintf(path, length, "%s/%s", directory, name);
        struct stat status;
        int exists = stat(path, &status) == 0;
        free(path);
        if (!exists) {
            ok = 0;
            break;
        }
        char kind = S_ISDIR(status.st_mode) ? 'd' : S_ISREG(status.st_mode) ? 'f' : 'o';
        size_t name_length = strlen(name);
        if (kind != 'd' && strcmp(name, "dmm.manifest") && (name_length < 4 || strcmp(name + name_length - 4, ".dmm")))
            continue;
        uint64_t value = hash(name) * UINT64_C(1099511628211);
        value = (value ^ (unsigned char)kind) * UINT64_C(1099511628211);
        sum += value;
        count++;
    }
    closedir(stream);
    *fingerprint = (sum ^ count) * UINT64_C(1099511628211);
    return ok;
}
static int capture_directory(void *context, const char *directory, int boundary_only) {
    IdeDirectoryInventory *inventory = context;
    uint64_t fingerprint;
    if (inventory->count >= 8192 || !directory_fingerprint(directory, boundary_only, &fingerprint))
        return 0;
    size_t length = strlen(directory) + 1;
    char *path = malloc(length);
    if (!path)
        return 0;
    memcpy(path, directory, length);
    IdeDirectory *grown = realloc(inventory->directories, (inventory->count + 1) * sizeof(*grown));
    if (!grown) {
        free(path);
        return 0;
    }
    inventory->directories = grown;
    grown[inventory->count++] = (IdeDirectory){path, fingerprint, boundary_only};
    return 1;
}
static int directories_current(const IdeDirectoryInventory *inventory) {
    for (size_t i = 0; i < inventory->count; i++) {
        const IdeDirectory *entry = &inventory->directories[i];
        uint64_t fingerprint;
        if (!directory_fingerprint(entry->path, entry->boundary_only, &fingerprint) ||
            fingerprint != entry->fingerprint)
            return 0;
    }
    return 1;
}
static void position(FILE *out, const char *source, AstSourceLocation location) {
    int row = 0, column = 0, points = 0;
    const unsigned char *p = (const unsigned char *)source;
    while (*p && row < location.line - 1) {
        if (*p++ == '\n')
            row++;
    }
    while (*p && *p != '\n' && *p != '\r' && points < location.column - 1) {
        unsigned c = *p++;
        int extra = c < 128 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
        for (int i = 0; i < extra && (*p & 0xc0) == 0x80; i++)
            p++;
        column += extra == 3 ? 2 : 1;
        points++;
    }
    fprintf(out, "{\"line\":%d,\"character\":%d}", row, column);
}
static void range(FILE *out, const char *source, AstSourceSpan span) {
    fputs("{\"start\":", out);
    position(out, source, span.begin);
    fputs(",\"end\":", out);
    position(out, source, span.end);
    fputc('}', out);
}
void driver_ide_capabilities(FILE *out) {
    fputs("{\"protocol\":\"dmm-ide-v1\",\"compilerVersion\":", out);
    ide_json_quote(out, DMM_VERSION);
    fputs(",\"editions\":[\"2026-10-04-dev\"],\"capabilities\":[\"workspace-"
          "analysis\",\"workspace-index\",\"directory-inventory\",\"manifest-overlays\",\"semantic-values\","
          "\"implementations\",\"completion\","
          "\"rename-local\","
          "\"manifest-sync-"
          "preview\",\"function-signatures\",\"type-inlays\",\"call-graph\"],"
          "\"debugFormats\":[],\"positionEncoding\":\"utf-16\"}\n",
          out);
}
static void diagnostic(FILE *out, const ErrorContext *error, const char *inherited, int *count) {
    const char *path = error->filename && *error->filename ? error->filename : inherited;
    char *canonical = path_identity_canonical(path);
    if (!canonical)
        return;
    char *source = read_source(canonical);
    if (!source)
        source = calloc(1, 1);
    if ((*count)++)
        fputc(',', out);
    fputs("{\"uri\":", out);
    uri(out, canonical);
    fprintf(out, ",\"code\":\"%c%03d\",\"severity\":%d,\"message\":", error->error_category, error->error_code,
            error->severity >= SEVERITY_ERROR     ? 1
            : error->severity == SEVERITY_WARNING ? 2
                                                  : 3);
    ide_json_quote(out, error->message);
    AstSourceSpan span = {
        {error->line, error->column},
        {error->end_line ? error->end_line : error->line, error->end_column ? error->end_column : error->column}};
    fputs(",\"range\":", out);
    range(out, source ? source : "", span);
    if (error->fix_replacement) {
        fputs(",\"fix\":{\"replacement\":", out);
        ide_json_quote(out, error->fix_replacement);
        fputs(",\"range\":", out);
        AstSourceSpan fix = {{error->fix_line, error->fix_column}, {error->fix_end_line, error->fix_end_column}};
        range(out, source ? source : "", fix);
        fputc('}', out);
    }
    fputc('}', out);
    for (int i = 0; i < error->child_count; i++)
        diagnostic(out, error->children[i], canonical, count);
    free(source);
    free(canonical);
}
static const char *kind(SemanticSymbolKind value) {
    static const char *names[] = {"function",          "struct",    "enum",  "interface", "import",   "field",
                                  "enum-value",        "parameter", "local", "constant",  "variable", "type-parameter",
                                  "lifetime-parameter"};
    return value <= SEMANTIC_SYMBOL_LIFETIME_PARAMETER ? names[value] : "unknown";
}
/* generic_origin on an aggregate method points at the aggregate, not the method. */
static const AstDeclarationNode *source_declaration(const AstDeclarationNode *decl) {
    if (decl && decl->source_origin)
        return source_declaration(decl->source_origin);
    for (unsigned depth = 0; decl && decl->generic_origin && depth < 64; depth++) {
        const AstDeclarationNode *parent = decl->generic_origin;
        if (decl->kind == AST_DECL_FUNCTION && parent->kind != AST_DECL_FUNCTION) {
            const AstDeclarationNode *method = parent->kind == AST_DECL_STRUCT      ? parent->as.struct_decl.methods
                                               : parent->kind == AST_DECL_ENUM      ? parent->as.enum_decl.methods
                                               : parent->kind == AST_DECL_INTERFACE ? parent->as.interface_decl.methods
                                                                                    : NULL;
            for (; method; method = method->next)
                if (method->name_token == decl->name_token)
                    break;
            if (!method)
                return decl;
            parent = method;
        }
        decl = parent;
    }
    return decl;
}
static size_t origin_depth(const SemanticModel *model, size_t id, unsigned depth) {
    if (id >= model->symbol_count)
        return AST_SYMBOL_NONE;
    if (depth > 64)
        return id;
    const SemanticSymbol *symbol = &model->symbols[id];
    if (symbol->source_only)
        return id;
    if (symbol->source_capture && symbol->source_capture->resolved_symbol_id != AST_SYMBOL_NONE)
        return origin_depth(model, symbol->source_capture->resolved_symbol_id, depth + 1);
    const AstDeclarationNode *decl = source_declaration(symbol->declaration);
    if (decl) {
        for (size_t i = 0; i < model->symbol_count; i++)
            if (model->symbols[i].source_only && model->symbols[i].kind == symbol->kind &&
                model->symbols[i].declaration == decl)
                return i;
        if (decl == symbol->declaration && !decl->generic_origin)
            return id;
        /* Prefer a registered source template; otherwise use its first checked instance. */
        for (size_t i = 0; i < model->symbol_count; i++)
            if (model->symbols[i].kind == symbol->kind && model->symbols[i].declaration == decl)
                return i;
        for (size_t i = 0; i < id; i++)
            if (model->symbols[i].kind == symbol->kind && model->symbols[i].source_program == symbol->source_program &&
                source_declaration(model->symbols[i].declaration) == decl)
                return i;
    } else if (symbol->owner_symbol_id < model->symbol_count) {
        const AstDeclarationNode *owner_decl = model->symbols[symbol->owner_symbol_id].declaration;
        size_t owner = origin_depth(model, symbol->owner_symbol_id, depth + 1);
        for (size_t i = 0; i < model->symbol_count; i++) {
            const SemanticSymbol *candidate = &model->symbols[i];
            if (candidate->source_only && candidate->kind == symbol->kind &&
                candidate->source_program == symbol->source_program && candidate->name_token == symbol->name_token &&
                candidate->owner_symbol_id == owner)
                return i;
        }
        if (!owner_decl || (!owner_decl->generic_origin && owner == symbol->owner_symbol_id))
            return id;
        /* Parameters/locals retain their exact source token through AST cloning. */
        for (size_t i = 0; i < id; i++) {
            const SemanticSymbol *candidate = &model->symbols[i];
            if (candidate->kind == symbol->kind && candidate->source_program == symbol->source_program &&
                candidate->name_token == symbol->name_token &&
                origin_depth(model, candidate->owner_symbol_id, depth + 1) == owner)
                return i;
        }
    }
    return id;
}
static size_t origin(const SemanticModel *model, size_t id) { return origin_depth(model, id, 0); }
static void declarations(FILE *out, const SemanticModel *model) {
    int count = 0;
    for (size_t i = 0; model && i < model->symbol_count; i++) {
        const SemanticSymbol *s = &model->symbols[i];
        const AstProgram *p = s->source_program;
        if (!p || s->name_token >= p->token_count || origin(model, i) != i)
            continue;
        const AstDeclarationNode *source_decl = source_declaration(s->declaration);
        const AstDeclarationNode *source_owner =
            s->owner_symbol_id < model->symbol_count
                ? source_declaration(model->symbols[s->owner_symbol_id].declaration)
                : NULL;
        int specialized = (source_decl && source_decl != s->declaration) ||
                          (source_owner && source_owner != model->symbols[s->owner_symbol_id].declaration);
        size_t name_token = source_decl
                                ? (source_decl->is_source_closure ? source_decl->first_token : source_decl->name_token)
                                : s->name_token;
        if (name_token >= p->token_count)
            continue;
        const AstToken *token = &p->tokens[name_token];
        if (token->span.begin.line < 1 || !token->lexeme || !*token->lexeme)
            continue;
        char *source = read_source(p->source_path);
        if (!source)
            continue;
        if (count++)
            fputc(',', out);
        fprintf(out, "{\"id\":%zu,\"kind\":", i);
        ide_json_quote(out, kind(s->kind));
        fputs(",\"name\":", out);
        ide_json_quote(out, token->lexeme);
        if (source_decl && source_decl->is_source_closure)
            fputs(",\"anonymous\":true,\"displayName\":\"<closure>\"", out);
        fputs(",\"uri\":", out);
        uri(out, p->source_path);
        fputs(",\"nameRange\":", out);
        range(out, source, token->span);
        fputs(",\"sourceRange\":", out);
        range(out, source, source_decl ? source_decl->span : token->span);
        fputs(",\"identity\":", out);
        char identity[100];
        snprintf(identity, sizeof(identity), "#%s:%d:%d", kind(s->kind), token->span.begin.line,
                 token->span.begin.column);
        size_t n = strlen(p->source_path) + strlen(identity) + 1;
        char *key = malloc(n);
        if (key) {
            snprintf(key, n, "%s%s", p->source_path, identity);
            ide_json_quote(out, key);
            free(key);
        } else
            fputs("null", out);
        if (s->owner_symbol_id < model->symbol_count)
            fprintf(out, ",\"owner\":%zu", origin(model, s->owner_symbol_id));
        fputs(",\"type\":", out);
        const AstType *source_type = NULL;
        if (specialized && source_decl && source_decl->kind == AST_DECL_FUNCTION)
            source_type = &source_decl->as.function.return_type;
        if (specialized && source_owner && source_owner->kind == AST_DECL_FUNCTION &&
            s->kind == SEMANTIC_SYMBOL_PARAMETER)
            for (const AstParameter *parameter = source_owner->as.function.parameters; parameter;
                 parameter = parameter->next)
                if (parameter->name_token == s->name_token) {
                    source_type = &parameter->type;
                    break;
                }
        if (specialized && source_owner && s->kind == SEMANTIC_SYMBOL_FIELD) {
            const AstField *field = source_owner->kind == AST_DECL_STRUCT ? source_owner->as.struct_decl.fields
                                    : source_owner->kind == AST_DECL_ENUM ? source_owner->as.enum_decl.fields
                                                                          : NULL;
            for (; field; field = field->next)
                if (field->name_token == s->name_token) {
                    source_type = &field->type;
                    break;
                }
        }
        char *display =
            source_type ? semantic_display_type(model, p, source_type) : semantic_display_symbol_type(model, s);
        ide_json_quote(out, s->kind == SEMANTIC_SYMBOL_TYPE_PARAMETER       ? token->lexeme
                            : s->kind == SEMANTIC_SYMBOL_LIFETIME_PARAMETER ? "lifetime"
                            : (specialized || s->source_only) && s->declared_type.kind == AST_TYPE_INFERRED ? "unknown"
                            : display                                                                       ? display
                                                                                                            : "");
        free(display);
        fprintf(out, ",\"inferred\":%s,\"specialized\":%s,\"sourceOnly\":%s",
                s->declared_type.kind == AST_TYPE_INFERRED ? "true" : "false", specialized ? "true" : "false",
                s->source_only ? "true" : "false");
        if (s->source_scope < model->source_scope_count) {
            fprintf(out, ",\"scope\":%zu", s->source_scope);
            const SemanticSourceScope *scope = &model->source_scopes[s->source_scope];
            if (scope->span.begin.line > 0) {
                fputs(",\"scopeRange\":", out);
                range(out, source, scope->span);
            }
        }
        if (source_decl) {
            fputs(",\"genericParameters\":[", out);
            int generics = 0;
            for (const AstGenericParameter *parameter = source_decl->generic_parameters; parameter;
                 parameter = parameter->next) {
                if (generics++)
                    fputc(',', out);
                ide_json_quote(out, ast_program_lexeme(p, parameter->name_token));
            }
            fputs("],\"lifetimeParameters\":[", out);
            int lifetimes = 0;
            for (const AstLifetimeParameter *parameter = source_decl->lifetime_parameters; parameter;
                 parameter = parameter->next) {
                if (lifetimes++)
                    fputc(',', out);
                ide_json_quote(out, ast_program_lexeme(p, parameter->name_token));
            }
            fputc(']', out);
        }
        if (s->kind == SEMANTIC_SYMBOL_FUNCTION && s->declaration) {
            const AstDeclarationNode *function = source_decl;
            fputs(",\"parameters\":[", out);
            int parameter_count = 0;
            for (const AstParameter *parameter = function->as.function.parameters; parameter;
                 parameter = parameter->next) {
                if (parameter->name_token >= p->token_count || p->tokens[parameter->name_token].span.begin.line < 1)
                    continue;
                if (parameter_count++)
                    fputc(',', out);
                fputs("{\"name\":", out);
                ide_json_quote(out, ast_program_lexeme(p, parameter->name_token));
                fputs(",\"type\":", out);
                char *parameter_type = semantic_display_type(model, p, &parameter->type);
                ide_json_quote(out, parameter_type ? parameter_type : "");
                free(parameter_type);
                fputc('}', out);
            }
            fputs("],\"returnType\":", out);
            char *return_type = semantic_display_type(model, p, &function->as.function.return_type);
            ide_json_quote(out, return_type ? return_type : "");
            free(return_type);
            fprintf(out, ",\"async\":%s", function->as.function.is_async ? "true" : "false");
            fprintf(out, ",\"static\":%s,\"declaredReceiverMode\":%u",
                    function->as.function.is_static ? "true" : "false", function->as.function.receiver_mode);
            unsigned receiver_mode;
            if (semantic_receiver_contract(model, i, &receiver_mode))
                fprintf(out, ",\"receiverMode\":%u", receiver_mode);
            else
                fputs(",\"receiverMode\":null", out);
        }
        fprintf(out,
                ",\"scopeDepth\":%zu,\"typeProperties\":%u,\"borrowKind\":%u,"
                "\"public\":%s}",
                s->scope_depth, s->type_properties, (unsigned)s->resolved_borrow_kind,
                s->declaration && s->declaration->is_public ? "true" : "false");
        free(source);
    }
}
static void implementation_relations(FILE *out, const SemanticModel *model) {
    size_t pairs = 0;
    int count = 0, complete = 1;
    size_t recorded_contexts = model ? model->interface_context_count : 0;
    int generic_complete = model && model->interface_discovery_coverage && !model->interface_contexts_incomplete;
    for (size_t i = 0; model && i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE && symbol->declaration &&
            !symbol->declaration->is_auto_interface && symbol->declaration->generic_parameters &&
            (i >= model->interface_discovery_count || !model->interface_discovery_coverage[i]))
            generic_complete = 0;
    }
    for (size_t i = 0; model && i < model->symbol_count && complete; i++) {
        const SemanticSymbol *interface = &model->symbols[i];
        if (interface->source_only || interface->kind != SEMANTIC_SYMBOL_INTERFACE || !interface->declaration ||
            interface->declaration->is_auto_interface)
            continue;
        int generic = interface->declaration->generic_parameters != NULL;
        size_t contexts = generic ? recorded_contexts : 1;
        for (size_t context_id = 0; context_id < contexts && complete; context_id++) {
            SemanticInterfaceContext saved_context = generic ? model->interface_contexts[context_id]
                                                            : (SemanticInterfaceContext){0};
            const SemanticInterfaceContext *context = generic ? &saved_context : NULL;
            if (context && context->interface_id != i) continue;
            AstType specialized = {.arguments = context ? context->arguments : NULL};
            for (size_t j = 0; j < model->symbol_count; j++) {
                const SemanticSymbol *implementation = &model->symbols[j];
                if ((implementation->source_only && (!context || context->source_owner != j)) ||
                    (context && context->source_owner != AST_SYMBOL_NONE && context->source_owner != j) ||
                    (implementation->kind != SEMANTIC_SYMBOL_STRUCT && implementation->kind != SEMANTIC_SYMBOL_ENUM) ||
                    !implementation->declaration || implementation->declaration->is_closure_environment)
                    continue;
                if (++pairs > 50000) {
                    complete = 0;
                    break;
                }
                if (!(context ? semantic_implements_interface_context(model, context, j)
                              : semantic_implements_interface(model, i, j)))
                    continue;
                if (count++)
                    fputc(',', out);
                fprintf(out, "{\"interface\":%zu,\"implementation\":%zu,\"checkedImplementation\":%zu",
                        origin(model, i), origin(model, j), j);
                if (context) fprintf(out, ",\"interfaceContext\":%zu", context_id);
                fputs(",\"methods\":[", out);
                int methods = 0;
                for (const AstDeclarationNode *required = interface->declaration->as.interface_decl.methods; required;
                     required = required->next) {
                    size_t actual = context ? semantic_specialized_interface_method(model, required->resolved_symbol_id,
                                                                                    j, context->argument_program,
                                                                                    &specialized)
                                            : semantic_interface_method(model, required->resolved_symbol_id, j);
                    if (actual == AST_SYMBOL_NONE)
                        continue;
                    if (methods++)
                        fputc(',', out);
                    fprintf(out, "{\"required\":%zu,\"implementation\":%zu}", origin(model, required->resolved_symbol_id),
                            origin(model, actual));
                }
                fputs("]}", out);
            }
        }
    }
    if (model && model->interface_context_count != recorded_contexts) complete = 0;
    fputs("],\"interfaceContexts\":[", out);
    for (size_t i = 0; model && i < model->interface_context_count; i++) {
        const SemanticInterfaceContext *context = &model->interface_contexts[i];
        if (i) fputc(',', out);
        fprintf(out, "{\"id\":%zu,\"interface\":%zu,\"wildcardParameters\":%u", i,
                origin(model, context->interface_id), context->wildcard_parameters);
        if (context->source_owner != AST_SYMBOL_NONE)
            fprintf(out, ",\"sourceOwner\":%zu", origin(model, context->source_owner));
        fputs(",\"argumentTypes\":[", out);
        int arguments = 0;
        for (const AstTypeArgument *argument = context->arguments; argument; argument = argument->next) {
            size_t index = (size_t)arguments++;
            if (index) fputc(',', out);
            if (context->wildcard_parameters & (1u << index)) {
                ide_json_quote(out, "*");
                continue;
            }
            char *display = semantic_display_type(model, context->argument_program, &argument->type);
            ide_json_quote(out, display ? display : "unknown");
            free(display);
        }
        fputs("],\"parameterBounds\":[", out);
        int parameters = 0;
        const SemanticSymbol *interface = &model->symbols[context->interface_id];
        for (const AstGenericParameter *parameter = interface->declaration->generic_parameters; parameter;
             parameter = parameter->next) {
            if (parameters++) fputc(',', out);
            fputc('[', out);
            int bounds = 0;
            for (const AstInterfaceBound *bound = parameter->bounds; bound; bound = bound->next) {
                if (bounds++) fputc(',', out);
                char *display = semantic_display_type(model, interface->source_program, &bound->type);
                ide_json_quote(out, display ? display : "unknown");
                free(display);
            }
            fputc(']', out);
        }
        fputs("]}", out);
    }
    fprintf(out, "],\"implementationCoverage\":{\"ordinary\":%s,\"genericInterfaces\":%s,"
                 "\"checkedGenericInterfaces\":%s,\"uncheckedInterfaces\":[", complete ? "true" : "false",
            generic_complete && complete && model && !model->error_count ? "true" : "false",
            complete && model && !model->error_count && !model->interface_contexts_incomplete ? "true" : "false");
    int unchecked = 0;
    for (size_t i = 0; model && i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->kind != SEMANTIC_SYMBOL_INTERFACE || !symbol->declaration ||
            symbol->declaration->is_auto_interface || !symbol->declaration->generic_parameters) continue;
        if (i < model->interface_discovery_count && model->interface_discovery_coverage[i]) continue;
        if (unchecked++) fputc(',', out);
        fprintf(out, "%zu", origin(model, i));
    }
    fputs("]}", out);
}

#include "ide_binding_coverage.inc"
typedef struct {
    FILE *out;
    const SemanticModel *model;
    const AstProgram *program;
    const char *source;
    int *count;
    int *incomplete;
    size_t function;
    int generated;
    size_t scope;
    size_t checked_context;
    IdeBindingAudit *audit;
    AstLifetimeOperation ownership_operation;
    AstBorrowKind access_borrow;
    AstAsyncOperation async_operation;
    int implicit_receiver;
    int intrinsic;
    int async_call;
    int awaiting;
    int call_arity;
    int source_candidate;
} Graph;
static void occurrence_span(Graph *g, AstSourceSpan span, size_t symbol, const char *role) {
    if (span.begin.line < 1)
        return;
    if ((*g->count)++)
        fputc(',', g->out);
    fputs("{\"uri\":", g->out);
    uri(g->out, g->program->source_path);
    fputs(",\"range\":", g->out);
    range(g->out, g->source, span);
    fputs(",\"role\":", g->out);
    ide_json_quote(g->out, role);
    symbol = origin(g->model, symbol);
    audit_binding(g->audit, g->program, span, role, origin(g->model, g->function),
                  g->intrinsic || source_symbol_anchor(g->model, symbol),
                  g->generated || g->checked_context != AST_SYMBOL_NONE, g->checked_context != AST_SYMBOL_NONE);
    if (symbol == AST_SYMBOL_NONE)
        fputs(",\"declaration\":null", g->out);
    else
        fprintf(g->out, ",\"declaration\":%zu", symbol);
    if (g->function != AST_SYMBOL_NONE)
        fprintf(g->out, ",\"caller\":%zu", origin(g->model, g->function));
    if (g->checked_context != AST_SYMBOL_NONE)
        fprintf(g->out, ",\"checkedContext\":%zu", g->checked_context);
    if (g->ownership_operation != LIFETIME_NONE)
        fprintf(g->out, ",\"ownershipOperation\":%u", (unsigned)g->ownership_operation);
    if (g->access_borrow != AST_BORROW_NONE)
        fprintf(g->out, ",\"accessBorrowKind\":%u", (unsigned)g->access_borrow);
    if (g->async_operation != ASYNC_NONE)
        fprintf(g->out, ",\"asyncOperation\":%u", (unsigned)g->async_operation);
    if (g->implicit_receiver)
        fputs(",\"implicitReceiver\":true", g->out);
    if (g->intrinsic)
        fputs(",\"intrinsic\":true", g->out);
    if (g->async_call)
        fputs(",\"asyncCall\":true", g->out);
    if (g->awaiting)
        fputs(",\"await\":true", g->out);
    if (g->source_candidate)
        fputs(",\"sourceCandidate\":true", g->out);
    fputc('}', g->out);
}
static void occurrence(Graph *g, size_t token, size_t symbol, const char *role) {
    if (token < g->program->token_count)
        occurrence_span(g, g->program->tokens[token].span, symbol, role);
}
static void statements(Graph *, const AstStatement *, unsigned);
static void declaration_uses(Graph *, const AstDeclarationNode *, unsigned);
static void type_uses(Graph *g, const AstType *type, unsigned depth) {
    /* A substituted type parameter is not a source reference to its concrete type. */
    if (g->generated)
        return;
    if (depth > 256) {
        if (g->incomplete)
            *g->incomplete = 1;
        return;
    }
    if (!type)
        return;
    size_t previous_scope = g->scope;
    size_t type_scope = semantic_source_scope(g->model, type);
    if (type_scope != AST_SYMBOL_NONE)
        g->scope = type_scope;
    if (type->kind != AST_TYPE_INFERRED && type->name_token < g->program->token_count) {
        size_t symbol = semantic_source_binding(g->model, g->program, g->scope, type->name_token,
                                                SEMANTIC_SYMBOL_TYPE_PARAMETER, type->name_span.begin);
        if (symbol == AST_SYMBOL_NONE)
            symbol = semantic_resolve_type_name(g->model, g->program, type->name_token);
        if (symbol != AST_SYMBOL_NONE || (type->kind == AST_TYPE_NAMED &&
                                         g->program->tokens[type->name_token].type == TOKEN_IDENTIFIER))
            occurrence_span(g, type->name_span, symbol, "type");
    }
    if (type->lifetime_token < g->program->token_count &&
        ast_program_lexeme(g->program, type->lifetime_token)[0] == '\'') {
        size_t symbol = semantic_source_binding(g->model, g->program, g->scope, type->lifetime_token,
                                                SEMANTIC_SYMBOL_LIFETIME_PARAMETER, type->span.begin);
        occurrence(g, type->lifetime_token, symbol, "lifetime");
    }
    for (const AstLifetimeParameter *lifetime = type->lifetime_arguments; lifetime; lifetime = lifetime->next) {
        size_t symbol = semantic_source_binding(g->model, g->program, g->scope, lifetime->name_token,
                                                SEMANTIC_SYMBOL_LIFETIME_PARAMETER, type->span.begin);
        occurrence(g, lifetime->name_token, symbol, "lifetime");
    }
    type_uses(g, type->reference_type, depth + 1);
    type_uses(g, type->element_type, depth + 1);
    for (const AstTypeArgument *a = type->arguments; a; a = a->next)
        type_uses(g, &a->type, depth + 1);
    for (const AstTypeArgument *a = type->function_parameters; a; a = a->next)
        type_uses(g, &a->type, depth + 1);
    type_uses(g, type->function_return_type, depth + 1);
    for (const AstGenericParameter *parameter = type->function_generic_parameters; parameter;
         parameter = parameter->next)
        for (const AstInterfaceBound *bound = parameter->bounds; bound; bound = bound->next)
            type_uses(g, &bound->type, depth + 1);
    g->scope = previous_scope;
}
static void expressions(Graph *g, const AstExpression *e, const char *role, unsigned depth) {
    if (depth > 256) {
        if (g->incomplete)
            *g->incomplete = 1;
        return;
    }
    for (; e; e = e->next) {
        int namespace_receiver = 0;
        int previous_arity = g->call_arity;
        AstLifetimeOperation previous_operation = g->ownership_operation;
        AstBorrowKind previous_borrow = g->access_borrow;
        AstAsyncOperation previous_async = g->async_operation;
        int previous_async_call = g->async_call, previous_await = g->awaiting;
        if (e->lifetime_operation != LIFETIME_NONE) g->ownership_operation = e->lifetime_operation;
        if (e->async_operation != ASYNC_NONE) g->async_operation = e->async_operation;
        if (e->kind == AST_EXPR_AWAIT) g->awaiting = 1;
        if (e->kind == AST_EXPR_CALL && e->resolved_symbol_id < g->model->symbol_count) {
            const SemanticSymbol *callee = &g->model->symbols[e->resolved_symbol_id];
            if (callee->kind == SEMANTIC_SYMBOL_FUNCTION && callee->declaration &&
                callee->declaration->as.function.is_async) g->async_call = 1;
        }
        if (e->kind == AST_EXPR_UNARY && e->operator_type == TOKEN_AMPERSAND)
            g->access_borrow = e->mutable_borrow ? AST_BORROW_MUTABLE : AST_BORROW_IMMUTABLE;
        type_uses(g, &e->allocated_type, depth + 1);
        if (e->kind == AST_EXPR_NAME || e->kind == AST_EXPR_MEMBER || e->kind == AST_EXPR_ENUM_ACCESS ||
            e->kind == AST_EXPR_ENUM_CONSTRUCT) {
            size_t symbol = e->resolved_symbol_id;
            if (symbol == AST_SYMBOL_NONE && e->kind == AST_EXPR_NAME)
                symbol = semantic_source_binding(g->model, g->program, g->scope, e->value_token, SEMANTIC_SYMBOL_LOCAL,
                                                 e->span.begin);
            if (symbol == AST_SYMBOL_NONE && e->kind == AST_EXPR_NAME)
                symbol = semantic_source_global_binding(g->model, g->program,
                                                        ast_program_lexeme(g->program, e->value_token));
            if (e->kind == AST_EXPR_MEMBER && e->left && e->left->kind == AST_EXPR_NAME &&
                semantic_source_binding(g->model, g->program, g->scope, e->left->value_token, SEMANTIC_SYMBOL_LOCAL,
                                        e->left->span.begin) == AST_SYMBOL_NONE) {
                const char *prefix = ast_program_lexeme(g->program, e->left->value_token);
                for (const AstDeclarationNode *decl = g->program->root; decl; decl = decl->next)
                    if (decl->kind == AST_DECL_IMPORT)
                        for (const AstImportPath *path = decl->as.import_decl.paths; path; path = path->next)
                            if (path->resolved_program &&
                                !strcmp(prefix, path->alias ? path->alias : path->resolved_program->package_name))
                                namespace_receiver = 1;
            }
            if (symbol == AST_SYMBOL_NONE && e->kind == AST_EXPR_MEMBER && e->left && e->left->kind == AST_EXPR_NAME &&
                semantic_source_binding(g->model, g->program, g->scope, e->left->value_token, SEMANTIC_SYMBOL_LOCAL,
                                        e->left->span.begin) == AST_SYMBOL_NONE &&
                semantic_source_global_binding(
                    g->model, g->program, ast_program_lexeme(g->program, e->left->value_token)) == AST_SYMBOL_NONE) {
                const char *prefix = ast_program_lexeme(g->program, e->left->value_token);
                const char *name = ast_program_lexeme(g->program, e->value_token);
                size_t length = strlen(prefix) + strlen(name) + 2;
                char *qualified = malloc(length);
                if (qualified) {
                    snprintf(qualified, length, "%s.%s", prefix, name);
                    symbol = semantic_source_global_binding(g->model, g->program, qualified);
                    free(qualified);
                }
            }
            const char *spelling = ast_program_lexeme(g->program, e->value_token);
            int intrinsic_literal = e->kind == AST_EXPR_NAME &&
                                    (!strcmp(spelling, "true") || !strcmp(spelling, "false"));
            g->implicit_receiver = e->kind == AST_EXPR_NAME && symbol < g->model->symbol_count &&
                                   g->model->symbols[symbol].kind == SEMANTIC_SYMBOL_FIELD &&
                                   !g->model->symbols[symbol].source_capture;
            g->intrinsic = symbol == AST_SYMBOL_NONE && e->kind == AST_EXPR_NAME &&
                           semantic_source_intrinsic(g->model, g->program, g->scope, e->value_token, e->span.begin);
            if (symbol == AST_SYMBOL_NONE && e->kind == AST_EXPR_MEMBER && !namespace_receiver) {
                size_t candidates[128];
                int complete = 1;
                size_t count = semantic_source_members(g->model, g->program, g->scope, e->left, e->value_token,
                                                        g->call_arity, candidates, 128, &complete);
                if (count == 1 && complete) symbol = candidates[0];
                else for (size_t i = 0; i < count; i++) {
                    g->source_candidate = 1;
                    occurrence_span(g, e->name_span.begin.line ? e->name_span : g->program->tokens[e->value_token].span,
                                    candidates[i], role);
                    g->source_candidate = 0;
                }
            }
            if (!intrinsic_literal) occurrence_span(g,
                            e->name_span.begin.line
                                ? e->name_span
                                : (e->value_token < g->program->token_count ? g->program->tokens[e->value_token].span
                                                                            : (AstSourceSpan){0}),
                            symbol, role);
            g->implicit_receiver = 0;
            g->intrinsic = 0;
        }
        if (e->initializer_name_token != AST_TOKEN_NONE)
            occurrence(g, e->initializer_name_token, e->initializer_field_symbol_id, "initializer-label");
        if (namespace_receiver)
            occurrence_span(g, e->left->name_span.begin.line ? e->left->name_span : e->left->span,
                            AST_SYMBOL_NONE, "namespace");
        else {
            g->call_arity = -1;
            if (e->kind == AST_EXPR_CALL) {
                g->call_arity = 0;
                for (const AstExpression *argument = e->arguments; argument; argument = argument->next) g->call_arity++;
            }
            expressions(g, e->left,
                    e->kind == AST_EXPR_CALL                                  ? "call"
                    : e->kind == AST_EXPR_MEMBER || e->kind == AST_EXPR_INDEX ? "read"
                                                                              : role,
                    depth + 1);
        }
        g->call_arity = -1;
        expressions(g, e->right,
                    e->kind == AST_EXPR_UNARY && e->operator_type == TOKEN_STAR && e->right &&
                            e->right->closure_capture_binding
                        ? role
                        : "read",
                    depth + 1);
        expressions(g, e->arguments, "read", depth + 1);
        statements(g, e->control, depth + 1);
        if (e->closure_environment) {
            const AstDeclarationNode *environment = e->closure_environment;
            const AstDeclarationNode *invoke = environment->closure_consuming_invoke
                                                   ? environment->closure_consuming_invoke
                                                   : environment->as.struct_decl.methods;
            size_t caller = g->function;
            if (invoke) {
                AstDeclarationNode callable = *invoke;
                callable.next = NULL;
                callable.source_origin = source_declaration(invoke);
                declaration_uses(g, &callable, depth + 1);
            }
            g->function = caller;
        }
        g->ownership_operation = previous_operation;
        g->access_borrow = previous_borrow;
        g->async_operation = previous_async;
        g->async_call = previous_async_call;
        g->awaiting = previous_await;
        g->call_arity = previous_arity;
    }
}
static void statements(Graph *g, const AstStatement *s, unsigned depth) {
    if (depth > 256) {
        if (g->incomplete)
            *g->incomplete = 1;
        return;
    }
    for (; s; s = s->next) {
        size_t previous_scope = g->scope;
        size_t statement_scope = semantic_source_scope(g->model, s->source_origin ? s->source_origin : s);
        if (statement_scope != AST_SYMBOL_NONE)
            g->scope = statement_scope;
        type_uses(g, &s->type, depth + 1);
        if (s->kind == AST_STMT_ASSIGNMENT && s->assignment_operator != TOKEN_EQUAL)
            expressions(g, s->expression, "read", depth + 1);
        expressions(g, s->expression, s->kind == AST_STMT_ASSIGNMENT ? "write" : "read", depth + 1);
        expressions(g, s->value, "read", depth + 1);
        expressions(g, s->condition, "read", depth + 1);
        expressions(g, s->update, "read", depth + 1);
        expressions(g, s->result, "read", depth + 1);
        statements(g, s->initializer, depth + 1);
        statements(g, s->body, depth + 1);
        statements(g, s->else_body, depth + 1);
        for (const AstMatchArm *a = s->match_arms; a; a = a->next) {
            size_t match_parent = g->scope;
            size_t arm_scope = semantic_source_scope(g->model, a);
            if (arm_scope != AST_SYMBOL_NONE)
                g->scope = arm_scope;
            type_uses(g, &a->type, depth + 1);
            for (const AstParameter *binding = a->bindings; binding; binding = binding->next)
                type_uses(g, &binding->type, depth + 1);
            if (!a->wildcard)
                occurrence(g, a->variant_token, a->resolved_variant_symbol, "read");
            statements(g, a->body, depth + 1);
            g->scope = match_parent;
        }
        g->scope = previous_scope;
    }
}
static void declaration_uses(Graph *g, const AstDeclarationNode *d, unsigned depth) {
    if (depth > 256) {
        if (g->incomplete)
            *g->incomplete = 1;
        return;
    }
    for (; d; d = d->next) {
        int previous_generated = g->generated;
        size_t previous_scope = g->scope;
        size_t previous_context = g->checked_context;
        AstLifetimeOperation previous_operation = g->ownership_operation;
        AstBorrowKind previous_borrow = g->access_borrow;
        AstAsyncOperation previous_async = g->async_operation;
        int previous_async_call = g->async_call, previous_await = g->awaiting;
        g->generated = previous_generated || d->generic_origin != NULL;
        size_t declaration_scope = semantic_source_scope(g->model, source_declaration(d));
        if (declaration_scope != AST_SYMBOL_NONE)
            g->scope = declaration_scope;
        for (const AstGenericParameter *parameter = d->generic_parameters; parameter; parameter = parameter->next)
            for (const AstInterfaceBound *bound = parameter->bounds; bound; bound = bound->next)
                type_uses(g, &bound->type, depth + 1);
        for (const AstAutoCondition *condition = d->where_conditions; condition; condition = condition->next) {
            type_uses(g, &condition->type, depth + 1);
            for (const AstInterfaceBound *bound = condition->bounds; bound; bound = bound->next)
                type_uses(g, &bound->type, depth + 1);
        }
        switch (d->kind) {
        case AST_DECL_FUNCTION:
            g->ownership_operation = LIFETIME_NONE;
            g->access_borrow = AST_BORROW_NONE;
            g->async_operation = ASYNC_NONE;
            g->async_call = g->awaiting = 0;
            g->function = d->resolved_symbol_id;
            g->checked_context = AST_SYMBOL_NONE;
            if (d->resolved_symbol_id < g->model->symbol_count) {
                const AstDeclarationNode *checked = g->model->symbols[d->resolved_symbol_id].declaration;
                if (checked && (checked->generic_origin || checked->source_origin))
                    g->checked_context = d->resolved_symbol_id;
            }
            if (declaration_scope != AST_SYMBOL_NONE)
                g->function = g->model->source_scopes[declaration_scope].owner;
            type_uses(g, &d->as.function.return_type, depth + 1);
            for (const AstParameter *parameter = d->as.function.parameters; parameter; parameter = parameter->next)
                type_uses(g, &parameter->type, depth + 1);
            statements(g, d->as.function.body, depth + 1);
            g->function = AST_SYMBOL_NONE;
            break;
        case AST_DECL_STRUCT:
            for (const AstField *field = d->as.struct_decl.fields; field; field = field->next)
                type_uses(g, &field->type, depth + 1);
            declaration_uses(g, d->as.struct_decl.methods, depth + 1);
            statements(g, d->as.struct_decl.destructor, depth + 1);
            break;
        case AST_DECL_ENUM:
            for (const AstField *field = d->as.enum_decl.fields; field; field = field->next)
                type_uses(g, &field->type, depth + 1);
            for (const AstEnumValue *variant = d->as.enum_decl.values; variant; variant = variant->next)
                for (const AstTypeArgument *a = variant->payload_types; a; a = a->next)
                    type_uses(g, &a->type, depth + 1);
            declaration_uses(g, d->as.enum_decl.methods, depth + 1);
            break;
        case AST_DECL_INTERFACE:
            declaration_uses(g, d->as.interface_decl.methods, depth + 1);
            break;
        case AST_DECL_CONSTANT:
        case AST_DECL_VARIABLE:
            type_uses(g, &d->as.constant.type, depth + 1);
            expressions(g, d->as.constant.value, "read", depth + 1);
            break;
        default:
            break;
        }
        g->generated = previous_generated;
        g->scope = previous_scope;
        g->checked_context = previous_context;
        g->ownership_operation = previous_operation;
        g->access_borrow = previous_borrow;
        g->async_operation = previous_async;
        g->async_call = previous_async_call;
        g->awaiting = previous_await;
    }
}
static int protected_path(const AstProgram *program, const char *path) {
    if (!program)
        return 0;
    if (path_identity_equal(program->source_path, path) != 0)
        return 1;
    for (size_t i = 0; i < program->loaded_source_count; i++)
        if (path_identity_equal(program->loaded_source_paths[i], path) != 0)
            return 1;
    for (size_t i = 0; i < program->owned_import_count; i++)
        if (protected_path(program->owned_imports[i], path))
            return 1;
    return 0;
}
static size_t utf16_offset(const char *text, int64_t line, int64_t column) {
    if (line < 0 || column < 0)
        return SIZE_MAX;
    size_t offset = 0;
    for (int64_t i = 0; i < line; i++) {
        const char *next = strchr(text + offset, '\n');
        if (!next)
            return SIZE_MAX;
        offset = (size_t)(next - text) + 1;
    }
    int64_t units = 0;
    while (units < column && text[offset] && text[offset] != '\n' && text[offset] != '\r') {
        unsigned char c = (unsigned char)text[offset];
        size_t width = c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        for (size_t i = 1; i < width; i++)
            if (!text[offset + i] || ((unsigned char)text[offset + i] & 0xc0) != 0x80)
                return SIZE_MAX;
        units += width == 4 ? 2 : 1;
        offset += width;
    }
    return units == column ? offset : SIZE_MAX;
}
static AstSourceLocation source_location(const char *text, size_t offset) {
    AstSourceLocation result = {1, 1};
    for (size_t i = 0; i < offset; i++)
        if (text[i] == '\n') {
            result.line++;
            result.column = 1;
        } else if (((unsigned char)text[i] & 0xc0) != 0x80)
            result.column++;
    return result;
}
static int identifier_byte(unsigned char c) { return isalnum(c) || c == '_' || c >= 128; }
#include "ide_rename.inc"
static char *ide_manifest_sync(const char *root) {
    char *directory = malloc(strlen(root) + 1);
    if (!directory)
        return NULL;
    strcpy(directory, root);
    char *slash = strrchr(directory, '/');
    if (!slash) {
        free(directory);
        return NULL;
    }
    *slash = 0;
    FILE *preview = tmpfile(), *out = tmpfile();
    char *path = NULL, *text = NULL, *source = NULL, *result = NULL;
    if (preview && out) {
        int ok = frontend_preview_manifest(directory, preview, &path);
        text = ok ? read_stream(preview) : NULL;
        source = path ? read_source(path) : NULL;
        if (ok && text && source) {
            fputs("{\"validated\":true,\"uri\":", out);
            uri(out, path);
            fputs(",\"oldText\":", out);
            ide_json_quote(out, source);
            fputs(",\"newText\":", out);
            ide_json_quote(out, text);
            fputc('}', out);
        } else
            fputs("{\"validated\":false,\"conflict\":\"Cannot reconcile the local dependency graph; resolve compiler "
                  "diagnostics first\"}",
                  out);
        result = read_stream(out);
    }
    if (preview)
        fclose(preview);
    if (out)
        fclose(out);
    free(directory);
    free(path);
    free(text);
    free(source);
    return result;
}
int driver_ide_request(const char *request_path, const char *response_path) {
    if (!request_path || !response_path || path_identity_equal(request_path, response_path) != 0)
        return 2;
    size_t response_length = strlen(response_path);
    if (response_length < 5 || strcmp(response_path + response_length - 5, ".json"))
        return 2;
    FILE *input = fopen(request_path, "rb");
    char *request_text = read_stream(input);
    if (input)
        fclose(input);
    JsonValue *request = request_text ? ide_json_parse(request_text) : NULL;
    const char *failure = NULL;
    char *root = NULL;
    char *completion_path = NULL, *completion_text = NULL, *completion_source = NULL;
    SemanticCompletionQuery query = {.marker = "__dmm_ide_completion__"};
    AstSourceSpan replacement = {0};
    DmmSourceOverlay overlays[1024] = {0};
    size_t overlay_count = 0;
    AstProgram *program = NULL;
    SemanticModel *model = NULL;
    IdeDirectoryInventory inventory = {0};
    FILE *reply = tmpfile();
    if (!reply) {
        free(request_text);
        ide_json_free(request);
        return 2;
    }
    const char *protocol = ide_json_string(request, "protocol"), *request_id = ide_json_string(request, "requestId"),
               *operation = ide_json_string(request, "operation");
    if (!protocol || strcmp(protocol, "dmm-ide-v1"))
        failure = "Unsupported or missing IDE protocol";
    else if (!request_id || !operation ||
             (strcmp(operation, "analyze") && strcmp(operation, "workspace-analysis") &&
              strcmp(operation, "complete") && strcmp(operation, "rename") && strcmp(operation, "sync-manifest")))
        failure = "Expected requestId and supported IDE operation";
    else if (!(root = uri_path(ide_json_string(request, "root"))))
        failure = "Expected a canonical local file URI root";
    for (int group = 0; !failure && group < 2; group++) {
        const JsonValue *list = ide_json_get(request, group ? "manifestOverlays" : "overlays");
        if (!list)
            continue;
        if (list->kind != JSON_ARRAY) {
            failure = "Overlays must be arrays";
            break;
        }
        for (const JsonValue *item = list->child; item; item = item->next) {
            const char *text = ide_json_string(item, "text");
            char *path = uri_path(ide_json_string(item, "uri"));
            int64_t version;
            if (!text || !path || !ide_json_integer(ide_json_get(item, "version"), &version) || version < 0 ||
                overlay_count == 1024) {
                free(path);
                failure = "Invalid overlay URI, text, version or overlay count";
                break;
            }
            size_t length = strlen(path);
            const char *leaf = strrchr(path, '/');
            if (group ? (!leaf || strcmp(leaf + 1, "dmm.manifest"))
                      : (length < 5 || strcmp(path + length - 4, ".dmm"))) {
                free(path);
                failure = "Overlay kind does not match source or manifest filename";
                break;
            }
            int duplicate = 0;
            for (size_t i = 0; i < overlay_count; i++)
                if (path_identity_equal(overlays[i].path, path) != 0)
                    duplicate = 1;
            if (duplicate) {
                free(path);
                failure = "Duplicate canonical overlay URI";
                break;
            }
            overlays[overlay_count++] = (DmmSourceOverlay){path, text, strlen(text)};
        }
    }
    ErrorHandler *handler = global_error_handler;
    handler->overlays = overlays;
    handler->overlay_count = overlay_count;
    handler->source_snapshot_enabled = 1;
    error_handler_set_json_output(handler, 1);
    error_handler_set_buffered(handler, 1);
    if (!failure && !strcmp(operation, "complete")) {
        completion_path = uri_path(ide_json_string(request, "uri"));
        int64_t line, column;
        const JsonValue *point = ide_json_get(request, "position");
        if (!completion_path || strlen(completion_path) < 4 ||
            strcmp(completion_path + strlen(completion_path) - 4, ".dmm") ||
            !ide_json_integer(ide_json_get(point, "line"), &line) ||
            !ide_json_integer(ide_json_get(point, "character"), &column) ||
            !(completion_source = read_source(completion_path)))
            failure = "Invalid completion URI or UTF-16 position";
        else {
            size_t cursor = utf16_offset(completion_source, line, column);
            if (cursor == SIZE_MAX)
                failure = "Completion position is outside the document or splits a surrogate pair";
            else {
                size_t start = cursor, end = cursor;
                while (start && identifier_byte((unsigned char)completion_source[start - 1]))
                    start--;
                while (identifier_byte((unsigned char)completion_source[end]))
                    end++;
                query.position = source_location(completion_source, start);
                query.source_path = completion_path;
                replacement = (AstSourceSpan){query.position, source_location(completion_source, end)};
                size_t length = strlen(completion_source), marker_length = strlen(query.marker);
                const char *after = completion_source + end;
                while (isspace((unsigned char)*after))
                    after++;
                int terminator = *after == '}' || !*after;
                completion_text = malloc(length + marker_length + 2);
                if (!completion_text)
                    failure = "Cannot allocate completion snapshot";
                else {
                    memcpy(completion_text, completion_source, start);
                    memcpy(completion_text + start, query.marker, marker_length);
                    size_t tail = start + marker_length;
                    if (terminator)
                        completion_text[tail++] = ';';
                    memcpy(completion_text + tail, completion_source + end, length - end + 1);
                    size_t index = 0;
                    for (; index < overlay_count; index++)
                        if (path_identity_equal(overlays[index].path, completion_path) == 1)
                            break;
                    if (index == overlay_count) {
                        if (overlay_count == 1024)
                            failure = "Too many overlays";
                        else {
                            size_t size = strlen(completion_path) + 1;
                            char *copy = malloc(size);
                            if (!copy)
                                failure = "Cannot allocate completion path";
                            else {
                                memcpy(copy, completion_path, size);
                                overlays[overlay_count++].path = copy;
                                handler->overlay_count = overlay_count;
                            }
                        }
                    }
                    if (!failure) {
                        overlays[index].text = completion_text;
                        overlays[index].length = strlen(completion_text);
                    }
                }
            }
        }
    }
    TargetFormat target = TARGET_ELF;
#ifdef _WIN32
    target = TARGET_COFF;
#endif
    const char *target_name = ide_json_string(request, "target");
    const JsonValue *stdlib_root = ide_json_get(request, "stdlibRoot");
    if (stdlib_root && stdlib_root->kind != JSON_STRING)
        failure = "stdlibRoot must be a source directory string";
    if (target_name) {
        if (!strcmp(target_name, "elf"))
            target = TARGET_ELF;
        else if (!strcmp(target_name, "coff"))
            target = TARGET_COFF;
        else
            failure = "Unsupported target";
    }
    char *manifest_sync = NULL;
    if (!failure && !strcmp(operation, "sync-manifest")) {
        manifest_sync = ide_manifest_sync(root);
        if (!manifest_sync)
            failure = "Cannot allocate manifest synchronization preview";
    }
    if (!failure && strcmp(operation, "sync-manifest")) {
        FrontendOptions options = {.recover_syntax = 1,
                                   .has_target = 1,
                                   .target_format = target,
                                   .system_packages = 1,
                                   .stdlib_root = ide_json_string(request, "stdlibRoot"),
                                   .workspace_directory =
                                       !strcmp(operation, "workspace-analysis") ? capture_directory : NULL,
                                   .workspace_directory_context = &inventory};
        program = frontend_parse_file(root, &options);
        if (program && !strcmp(operation, "workspace-analysis") && !frontend_discover_workspace(program, &options))
            failure = "Compiler workspace discovery failed; resolve module diagnostics or discovery limits";
        if (!failure && program && ast_validate_program(program))
            model = semantic_analyze_target_ide(program, target, completion_path ? &query : NULL);
    }
    int conflict = (root && path_identity_equal(root, response_path) != 0) || protected_path(program, response_path);
    for (size_t i = 0; i < overlay_count; i++)
        if (path_identity_equal(overlays[i].path, response_path) != 0)
            conflict = 1;
    for (size_t i = 0; i < handler->source_snapshot_count; i++)
        if (handler->source_snapshot[i].text &&
            path_identity_equal(handler->source_snapshot[i].path, response_path) != 0)
            conflict = 1;
    if (conflict) {
        failure = "IDE response must differ from every input source or manifest";
    }
    if (handler->source_snapshot_failed)
        failure = "Cannot capture a complete workspace snapshot within IDE resource limits";
    char *rename = NULL;
    const char *rename_conflict = NULL;
    if (!failure && !strcmp(operation, "rename"))
        rename = ide_rename(request, program, model, target, &rename_conflict);
    if (!failure && model && !semantic_register_source_graph(model))
        failure = "Cannot register source declarations within IDE resource limits";
    if (!failure && model)
        semantic_discover_interface_contexts(model);
    if (!failure && !directories_current(&inventory))
        failure = "Workspace directory inventory changed during analysis; retry the request";
    fputs("{\"protocol\":\"dmm-ide-v1\",\"requestId\":", reply);
    ide_json_quote(reply, request_id);
    if (failure) {
        fputs(",\"error\":{\"code\":\"invalid-request\",\"message\":", reply);
        ide_json_quote(reply, failure);
        fputs("}}\n", reply);
    } else {
        if (manifest_sync) {
            fputs(",\"manifestSync\":", reply);
            fputs(manifest_sync, reply);
        }
        if (!strcmp(operation, "rename")) {
            fputs(",\"rename\":", reply);
            if (rename)
                fputs(rename, reply);
            else {
                fputs("{\"validated\":false,\"conflict\":", reply);
                ide_json_quote(reply, rename_conflict);
                fputc('}', reply);
            }
        }
        uint64_t snapshot = hash(root);
        fputs(",\"dependencies\":[", reply);
        int dependencies = 0;
        size_t cached = handler->source_snapshot_count;
        size_t loaded = program ? program->loaded_source_count : cached + overlay_count;
        for (size_t i = 0; i <= loaded; i++) {
            const char *path = i == loaded  ? root
                               : program    ? program->loaded_source_paths[i]
                               : i < cached ? handler->source_snapshot[i].path
                                            : overlays[i - cached].path;
            int duplicate = 0;
            if (i == loaded && program)
                for (size_t j = 0; j < loaded; j++)
                    if (!strcmp(program->loaded_source_paths[j], root))
                        duplicate = 1;
            if (!program) {
                for (size_t j = 0; j < i; j++) {
                    const char *previous = j < cached ? handler->source_snapshot[j].path : overlays[j - cached].path;
                    if (!strcmp(previous, path)) {
                        duplicate = 1;
                        break;
                    }
                }
            }
            if (duplicate)
                continue;
            char *text = read_source(path);
            if (!text)
                continue;
            uint64_t fingerprint = hash(text);
            snapshot = (snapshot ^ hash(path) ^ fingerprint) * UINT64_C(1099511628211);
            if (dependencies++)
                fputc(',', reply);
            fputs("{\"uri\":", reply);
            uri(reply, path);
            fprintf(reply, ",\"hash\":\"fnv1a64:%016" PRIx64 "\"}", fingerprint);
            free(text);
        }
        fputs("],\"directoryDependencies\":[", reply);
        for (size_t i = 0; i < inventory.count; i++) {
            const IdeDirectory *entry = &inventory.directories[i];
            snapshot = (snapshot ^ hash(entry->path) ^ entry->fingerprint) * UINT64_C(1099511628211);
            if (i)
                fputc(',', reply);
            fputs("{\"uri\":", reply);
            uri(reply, entry->path);
            fprintf(reply, ",\"hash\":\"fnv1a64-inventory:%016" PRIx64 "\",\"boundaryOnly\":%s}", entry->fingerprint,
                    entry->boundary_only ? "true" : "false");
        }
        fprintf(reply, "],\"snapshotId\":\"%016" PRIx64 "\",\"workspaceDiscoveryComplete\":%s", snapshot,
                !strcmp(operation, "workspace-analysis") ? "true" : "false");
        if (program && program->module) {
            fputs(",\"workspaceRoot\":", reply);
            uri(reply, program->module->root);
            fputs(",\"workspaceRoots\":[", reply);
            int roots = 0;
            for (const DmmPackage *package = program->module->graph->packages; package; package = package->next) {
                if (package->module != program->module || !package->file_count)
                    continue;
                if (roots++)
                    fputc(',', reply);
                uri(reply, package->files[0]->source_path);
            }
            fputc(']', reply);
        }
        fputs(",\"diagnostics\":[", reply);
        int count = 0;
        for (int i = 0; i < handler->buffer_count; i++)
            diagnostic(reply, handler->buffer[i], root, &count);
        fputs("],\"declarations\":[", reply);
        declarations(reply, model);
        fputs("],\"implementationRelations\":[", reply);
        implementation_relations(reply, model);
        fputs(",\"occurrences\":[", reply);
        count = 0;
        int graph_incomplete = 0;
        IdeBindingAudit binding_audit = {0};
        if (model) {
            for (size_t i = 0; i <= program->owned_import_count; i++) {
                const AstProgram *p = i == 0 ? program : program->owned_imports[i - 1];
                char *source = read_source(p->source_path);
                if (source) {
                    Graph graph = {.out = reply, .model = model, .program = p, .source = source,
                                   .count = &count, .incomplete = &graph_incomplete,
                                   .function = AST_SYMBOL_NONE, .scope = semantic_source_scope(model, p),
                                   .checked_context = AST_SYMBOL_NONE, .audit = &binding_audit, .call_arity = -1};
                    declaration_uses(&graph, p->root, 0);
                    free(source);
                } else graph_incomplete = 1;
            }
        }
        fputs("],\"checkedContexts\":[", reply);
        int contexts = 0;
        for (size_t i = 0; model && i < model->symbol_count; i++) {
            const SemanticSymbol *symbol = &model->symbols[i];
            const AstDeclarationNode *checked = symbol->declaration;
            if (symbol->kind != SEMANTIC_SYMBOL_FUNCTION || !checked ||
                (!checked->generic_origin && !checked->source_origin))
                continue;
            if (contexts++)
                fputc(',', reply);
            fprintf(reply, "{\"id\":%zu,\"sourceDeclaration\":%zu,\"parameterTypes\":[", i, origin(model, i));
            int parameters = 0;
            for (const AstParameter *parameter = checked->as.function.parameters; parameter;
                 parameter = parameter->next) {
                if (parameters++)
                    fputc(',', reply);
                char *display = semantic_display_type(model, symbol->source_program, &parameter->type);
                ide_json_quote(reply, display ? display : "unknown");
                free(display);
            }
            fputs("],\"returnType\":", reply);
            char *display = semantic_display_type(model, symbol->source_program, &checked->as.function.return_type);
            ide_json_quote(reply, display ? display : "unknown");
            free(display);
            fputc('}', reply);
        }
        fprintf(reply,
                "],\"graphTraversalComplete\":%s,\"sourceRegistryComplete\":%s,"
                "\"bindingCoverage\":",
                graph_incomplete ? "false" : "true", model && model->source_registry_complete ? "true" : "false");
        binding_coverage(reply, &binding_audit, model, !graph_incomplete);
        fprintf(reply, ",\"completionResolved\":%s,\"items\":[", model && model->completion_found ? "true" : "false");
        for (size_t i = 0; model && i < model->completion_count; i++) {
            size_t id = model->completion_symbols[i];
            const SemanticSymbol *candidate = &model->symbols[id];
            if (i)
                fputc(',', reply);
            fprintf(reply, "{\"declaration\":%zu,\"label\":", origin(model, id));
            ide_json_quote(reply, ast_program_lexeme(candidate->source_program, candidate->name_token));
            fputs(",\"kind\":", reply);
            ide_json_quote(reply, kind(candidate->kind));
            fputs(",\"detail\":", reply);
            char *type = semantic_display_symbol_type(model, candidate);
            ide_json_quote(reply, type ? type : "");
            free(type);
            fputs(",\"replacementRange\":", reply);
            range(reply, completion_source ? completion_source : "", replacement);
            fputc('}', reply);
        }
        fputs("],\"diagnosticsJson\":", reply);
        FILE *legacy = tmpfile();
        if (legacy) {
            FILE *old = handler->output_stream;
            handler->output_stream = legacy;
            error_handler_flush(handler);
            handler->output_stream = old;
            char *text = read_stream(legacy);
            ide_json_quote(reply, text ? text : "");
            free(text);
            fclose(legacy);
        } else
            ide_json_quote(reply, "");
        fputs(",\"ast\":", reply);
        FILE *ast = tmpfile();
        if (ast) {
            if (model)
                ast_dump(ast, program);
            char *text = read_stream(ast);
            ide_json_quote(reply, text ? text : "");
            free(text);
            fclose(ast);
        } else
            ide_json_quote(reply, "");
        fputs("}\n", reply);
    }
    int result = 2;
    if (!conflict) {
        char *text = read_stream(reply);
        FILE *out = text ? fopen(response_path, "wb") : NULL;
        if (out) {
            int ok = fwrite(text, 1, strlen(text), out) == strlen(text);
            if (fclose(out))
                ok = 0;
            result = ok ? (failure ? 2 : 0) : 2;
        }
        free(text);
    }
    fclose(reply);
    free(rename);
    for (size_t i = 0; i < inventory.count; i++)
        free(inventory.directories[i].path);
    free(inventory.directories);
    free(manifest_sync);
    semantic_model_free(model);
    ast_program_free(program);
    handler->overlays = NULL;
    handler->overlay_count = 0;
    error_handler_clear_source_snapshot(handler);
    for (size_t i = 0; i < overlay_count; i++)
        free((void *)overlays[i].path);
    free(root);
    free(completion_path);
    free(completion_text);
    free(completion_source);
    free(request_text);
    ide_json_free(request);
    return result;
}
