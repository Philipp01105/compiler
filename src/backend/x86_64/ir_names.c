#include "ir_names.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const IrParameter *function_receiver(const IrFunction *function) {
    return function != NULL && function->parameter_count != 0 &&
           function->parameters[0].is_receiver
               ? &function->parameters[0]
               : NULL;
}

static int runtime_link_name(const char *name) {
    if (!strncmp(name, "__dmm_", 6)) return 1;
    static const char *runtime_names[] = {
        "printf", "putchar", "puts", "strcmp", "strcpy", "strcat", "strdup",
        "_strdup", "malloc", "calloc", "free", "strlen", "strtoll", "scanf",
        "snprintf", "fflush", "read", "write", "open", "close", "exit",
        "_read", "_write", "_open", "_close", "_snprintf", "_strtoi64",
        "__errno_location", "_errno", "__libc_start_main", "__isoc99_scanf",
        "VirtualAlloc", "VirtualFree", "GetStdHandle", "ReadFile", "WriteFile",
        "CreateFileA", "CloseHandle", "GetLastError", "ExitProcess"
    };
    for (size_t i = 0; i < sizeof(runtime_names) / sizeof(runtime_names[0]); i++)
        if (strcmp(name, runtime_names[i]) == 0) return 1;
    return 0;
}

int mangle_append(char *buffer, size_t buffer_size, size_t *used,
                         const char *text) {
    size_t length = strlen(text);
    if (*used > buffer_size || length >= buffer_size - *used) return 0;
    memcpy(buffer + *used, text, length + 1U);
    *used += length;
    return 1;
}

static int mangle_type(const IrModule *module, IrTypeId type_id, char *buffer,
                       size_t buffer_size, size_t *used, size_t depth) {
    if (type_id >= module->type_count || depth > module->type_count) return 0;
    const IrType *type = &module->types[type_id];
    char part[96];
    if (type->kind == IR_TYPE_PRIMITIVE) {
        static const char *const codes[] = {
            "i", "c", "y", "b", "f", "d", "s",
            "t2_i8", "t2_u8", "t3_i16", "t3_u16", "t3_i32", "t3_u32",
            "t3_i64", "t3_u64", "t5_isize", "t5_usize", "v", "n"
        };
        if (type->primitive < TYPE_INT || type->primitive > TYPE_NEVER) return 0;
        return mangle_append(buffer, buffer_size, used, codes[type->primitive]);
    }
    if (type->kind==IR_TYPE_EXECUTOR) return mangle_append(buffer,buffer_size,used,"e");
    if (type->kind == IR_TYPE_FUTURE || type->kind==IR_TYPE_JOIN)
        return mangle_append(buffer, buffer_size, used, type->kind==IR_TYPE_JOIN ? "j":"h") &&
               mangle_type(module, type->element_type, buffer, buffer_size, used, depth + 1U);
    if (type->kind == IR_TYPE_POINTER)
        return mangle_append(buffer, buffer_size, used, "p") &&
               mangle_type(module, type->element_type, buffer, buffer_size, used, depth + 1U);
    if (type->kind == IR_TYPE_ARRAY) {
        (void) snprintf(part, sizeof(part), "a%zu_", type->array_length);
        return mangle_append(buffer, buffer_size, used, part) &&
               mangle_type(module, type->element_type, buffer, buffer_size, used, depth + 1U);
    }
    if (type->kind == IR_TYPE_SLICE)
        return mangle_append(buffer, buffer_size, used, "l") &&
               mangle_type(module, type->element_type, buffer, buffer_size, used, depth + 1U);
    if (type->kind == IR_TYPE_FUNCTION) {
        if (type->signature_id >= module->signature_count ||
            !mangle_append(buffer, buffer_size, used, "q")) return 0;
        const IrFunctionSignature *signature = &module->signatures[type->signature_id];
        (void) snprintf(part, sizeof(part), "%zu_", signature->parameter_count);
        if (!mangle_append(buffer, buffer_size, used, part)) return 0;
        for (size_t i = 0; i < signature->parameter_count; i++)
            if (!mangle_type(module, signature->parameter_types[i], buffer,
                             buffer_size, used, depth + 1U)) return 0;
        return mangle_append(buffer, buffer_size, used, "r") &&
               mangle_type(module, signature->return_type, buffer,
                           buffer_size, used, depth + 1U);
    }
    if (type->kind != IR_TYPE_NAMED || type->symbol_id >= module->semantics->symbol_count)
        return 0;
    const SemanticSymbol *symbol = &module->semantics->symbols[type->symbol_id];
    const char *identity = symbol->source_program->module_identity == NULL
                               ? "."
                               : symbol->source_program->module_identity;
    const char *name = ast_program_lexeme(symbol->source_program, symbol->name_token);
    (void) snprintf(part, sizeof(part), "n%zu_", strlen(identity));
    if (!mangle_append(buffer, buffer_size, used, part)) return 0;
    for (const unsigned char *p = (const unsigned char *) identity; *p != '\0'; p++) {
        (void) snprintf(part, sizeof(part), "%02x", *p);
        if (!mangle_append(buffer, buffer_size, used, part)) return 0;
    }
    (void) snprintf(part, sizeof(part), "_%zu_", strlen(name));
    return mangle_append(buffer, buffer_size, used, part) &&
           mangle_append(buffer, buffer_size, used, name);
}

static int function_is_overloaded(const IrModule *module, const IrFunction *function) {
    if (function->is_drop_glue || function->is_package_init ||
        function->is_package_cleanup) return 0;
    const char *name = ast_program_lexeme(function->source_program, function->name_token);
    size_t matches = 0;
    for (size_t i = 0; i < module->function_count; i++) {
        const IrFunction *candidate = &module->functions[i];
        if (candidate->owner_symbol_id != function->owner_symbol_id) continue;
        if (function->owner_symbol_id != AST_SYMBOL_NONE) {
            const SemanticSymbol *left = &module->semantics->symbols[function->symbol_id];
            const SemanticSymbol *right = &module->semantics->symbols[candidate->symbol_id];
            if (left->declaration->as.function.is_static !=
                right->declaration->as.function.is_static)
                continue;
        }
        if (strcmp(ast_program_lexeme(candidate->source_program, candidate->name_token),
                   name) == 0)
            matches++;
    }
    return matches > 1;
}

const char *function_link_name(const IrModule *module, const IrFunction *function,
                                      char *buffer, size_t buffer_size) {
    if (function->is_package_init) return "__dmm_package_init";
    if (function->is_package_cleanup) return "__dmm_package_cleanup";
    if (function->interface_thunk_symbol_id != AST_SYMBOL_NONE) {
        (void) snprintf(buffer, buffer_size, "__dmm_interface_thunk_%zu",
                        function->interface_thunk_symbol_id);
        return buffer;
    }
    if (function->is_drop_glue) {
        IrTypeId owner_type = IR_TYPE_NONE;
        for (IrTypeId t = 0; t < module->type_count; t++)
            if (module->types[t].kind == IR_TYPE_NAMED &&
                module->types[t].symbol_id == function->owner_symbol_id) {
                owner_type = t;
                break;
            }
        size_t used = 0;
        buffer[0] = '\0';
        return mangle_append(buffer, buffer_size, &used, "__dmm_drop_") &&
               mangle_type(module, owner_type, buffer, buffer_size, &used, 0)
                   ? buffer
                   : NULL;
    }
    if (function->symbol_id < module->semantics->symbol_count) {
        const AstDeclarationNode *declaration = module->semantics->symbols[function->symbol_id].declaration;
        if (declaration != NULL && declaration->specialization_identity != NULL)
            return declaration->specialization_identity;
    }

    const char *name = ast_program_lexeme(function->source_program, function->name_token);
    if (strcmp(name, "main") == 0 && (!function->source_program->package_name ||
                                      !strcmp(function->source_program->package_name, "main")))
        return name;
    if (function->owner_symbol_id == AST_SYMBOL_NONE &&
        !function->source_program->package && !function_is_overloaded(module, function) && !runtime_link_name(name))
        return name;
    size_t used = 0;
    buffer[0] = '\0';
    char part[128];
    if (function->owner_symbol_id == AST_SYMBOL_NONE) {
        const char *identity = function->source_program->module_identity;
        if (identity && function->source_program->package) {
            if (!mangle_append(buffer, buffer_size, &used, "__dmm_p")) return NULL;
            for (const unsigned char *p = (const unsigned char *) identity; *p; p++) {
                snprintf(part, sizeof(part), "%02x", *p);
                if (!mangle_append(buffer, buffer_size, &used, part)) return NULL;
            }
            if (!mangle_append(buffer, buffer_size, &used, "_")) return NULL;
        }
        (void) snprintf(part, sizeof(part), "__dmm_f%zu_", strlen(name));
        if (!mangle_append(buffer, buffer_size, &used, part) ||
            !mangle_append(buffer, buffer_size, &used, name))
            return NULL;
        (void) snprintf(part, sizeof(part), "__%zu", function->parameter_count);
        if (!mangle_append(buffer, buffer_size, &used, part)) return NULL;
    } else {
        IrTypeId owner_type = IR_TYPE_NONE;
        for (IrTypeId t = 0; t < module->type_count; t++)
            if (module->types[t].kind == IR_TYPE_NAMED &&
                module->types[t].symbol_id == function->owner_symbol_id) {
                owner_type = t;
                break;
            }
        if (!mangle_append(buffer, buffer_size, &used, "__dmm_m") ||
            !mangle_type(module, owner_type, buffer, buffer_size, &used, 0))
            return NULL;
        const SemanticSymbol *symbol = &module->semantics->symbols[function->symbol_id];
        (void) snprintf(part, sizeof(part), "_%c_%zu_",
                        symbol->declaration->as.function.is_static ? 's' : 'i',
                        strlen(name));
        if (!mangle_append(buffer, buffer_size, &used, part) ||
            !mangle_append(buffer, buffer_size, &used, name))
            return NULL;
        (void) snprintf(part, sizeof(part), "__%zu", function->parameter_count -
                                                     (function_receiver(function) != NULL ? 1U : 0U));
        if (!mangle_append(buffer, buffer_size, &used, part)) return NULL;
    }
    size_t first = function_receiver(function) != NULL ? 1U : 0U;
    for (size_t i = first; i < function->parameter_count; i++)
        if (!mangle_append(buffer, buffer_size, &used, "_") ||
            !mangle_type(module, function->parameters[i].type_id, buffer,
                         buffer_size, &used, 0))
            return NULL;
    return buffer;
}

static int compare_link_names(const void *left, const void *right) {
    return strcmp(*(const char *const *) left, *(const char *const *) right);
}

int valid_module(const IrModule *module) {
    if (module == NULL || !module->verified || module->program == NULL ||
        module->semantics == NULL || (module->function_count == 0 &&
                                      (!module->program->package_name || !
                                       strcmp(module->program->package_name, "main"))))
        return 0;
    if (module->function_count == 1) {
        char buffer[4096];
        if (function_link_name(module, &module->functions[0], buffer, sizeof(buffer)) == NULL) return 0;
    } else if (module->function_count > 1) {
        if (module->function_count > SIZE_MAX / sizeof(char *)) return 0;
        char **names = calloc(module->function_count, sizeof(*names));
        if (names == NULL) return 0;
        int valid = 1;
        for (size_t i = 0; i < module->function_count; i++) {
            char buffer[4096];
            const char *name = function_link_name(module, &module->functions[i], buffer, sizeof(buffer));
            if (name == NULL) {
                valid = 0;
                break;
            }
            size_t length = strlen(name);
            names[i] = malloc(length + 1);
            if (names[i] == NULL) {
                valid = 0;
                break;
            }
            memcpy(names[i], name, length + 1);
        }
        if (valid) {
            qsort(names, module->function_count, sizeof(*names), compare_link_names);
            for (size_t i = 1; i < module->function_count; i++)
                if (strcmp(names[i - 1], names[i]) == 0) {
                    valid = 0;
                    break;
                }
        }
        for (size_t i = 0; i < module->function_count; i++) free(names[i]);
        free(names);
        if (!valid) return 0;
    }
    const SemanticSymbol *main_symbol = semantic_find_global(module->semantics, "main",
                                                             SEMANTIC_SYMBOL_FUNCTION);
    if (module->program->package_name && strcmp(module->program->package_name, "main")) return 1;
    return main_symbol != NULL && main_symbol->declaration != NULL &&
           main_symbol->declaration->as.function.parameters == NULL;
}
