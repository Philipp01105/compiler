#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include "frontend.h"
#include "ast_optimize.h"
#include "ir.h"
#include "ir_cfg.h"
#include "ir_optimize.h"
#include "semantic.h"
#include "backend.h"
#include "errorHandler.h"
#include "path_identity.h"
#include "external_link.h"

#ifndef DMM_VERSION
#define DMM_VERSION "development"
#endif

/*
 * print_usage - Display command line usage information
 * @program_name: Name of the program
 */
static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] <source_file>\n", program_name);
    printf("       %s manifest sync [package_directory] [--formatError]\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --tokens       Show generated token stream\n");
    printf("  --debug        Enable debug output during parsing\n");
    printf("  --formatError  Output errors in JSON format\n");
    printf("  --ide          Recover syntax for editor analysis; requires --dump-ast, emits no program\n");
    printf("  --ide-buffer FILE  Read editor contents from FILE while keeping the source path/imports (--ide only)\n");
    printf("  --emit=MODE    Output exe (default), obj, or asm\n");
    printf("  --runtime-component Build a private runtime object without application startup/runtime\n");
    printf("  --link=MODE    auto (default), internal, external; external requests platform runtime\n");
    printf("                 For obj/asm selects the future link ABI; starts no link process\n");
    printf("  --linker-driver PATH  GCC-compatible driver (default: gcc); exe only\n");
    printf("  --runtime-shim PATH   Complete private platform/network shim object; exe only\n");
    printf("  --native-library NAME=PATH  Override a logical native library\n");
    printf("  --native-library-dir DIR    Add a native library search directory (repeatable)\n");
    printf("  --dump-native-link FILE     Write target/profile/used native import requirements\n");
    printf("                 Used network operations select platform runtime automatically\n");
    printf("  -c             Emit a native object file (same as --emit=obj)\n");
    printf("  -S             Emit assembly (same as --emit=asm)\n");
    printf("  -O0 / -O1      Disable / enable AST and IR optimization (default: -O1)\n");
    printf("  --syntax=MODE  Assembly printing syntax: att or intel (default: intel)\n");
    printf("  --target=FMT   Target format: elf or coff (default: auto-detect)\n");
    printf("  --dump-ast FILE Write the stable dmm-ast-v5 dump to FILE\n");
    printf("  --dump-tokens FILE Write the token inventory to FILE\n");
    printf("  --dump-symbols FILE Write the semantic symbol table to FILE\n");
    printf("  --dump-ir-before-opt FILE Write lowered IR before IR passes to FILE\n");
    printf("  --dump-ir-passes FILE Trace every optimization pass and iteration to FILE\n");
    printf("  --dump-ir FILE  Write the selected typed IR to FILE\n");
    printf("  --dump-cfg FILE Write the selected control-flow graph to FILE\n");
    printf("  --source-map FILE Write the instruction source map to FILE\n");
    printf("  -o FILE        Write the selected output to FILE\n");
    printf("  --deterministic Produce reproducible output\n");
    printf("  --help         Show this help message\n");
    printf("  --version      Show compiler version\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s program.txt\n", program_name);
    printf("  %s --tokens program.txt\n", program_name);
    printf("  %s --debug program.txt\n", program_name);
    printf("  %s -c --target=coff program.dmm -o program.obj\n", program_name);
    printf("  %s -S --syntax=att --target=elf program.dmm\n", program_name);
    printf("\n");
}

static void print_header(const char *source_file) {
    struct stat st = {0};
    (void) stat(source_file, &st);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char datetime[64];
    strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S", t);

    printf("\n");
    printf("################################################################\n");
    printf("#                                                              #\n");
    printf("#                      DMM COMPILER                            #\n");
    printf("#                                                              #\n");
    printf("################################################################\n");
    printf("\n");
    printf("  File:    %s\n", source_file);
    printf("  Size:    %ld Bytes\n", st.st_size);
    printf("  Date:    %s\n", datetime);
    printf("\n");
}

static int artifact_conflicts_with_program(const AstProgram *program, const char *path);

static void remove_stale_output(const AstProgram *program, const char *source_file, const char *requested_output,
                                const char *const *artifacts, size_t artifact_count, const char *suffix) {
    /* Never delete an explicit path before validating it as a safe output. */
    if (requested_output != NULL) return;
    size_t length = strlen(source_file);
    size_t suffix_length = strlen(suffix) + 1;
    if (length > SIZE_MAX - suffix_length) return;
    char *path = malloc(length + suffix_length);
    if (path == NULL) return;
    memcpy(path, source_file, length);
    memcpy(path + length, suffix, suffix_length);
    int conflict = artifact_conflicts_with_program(program, path);
    for (size_t i = 0; i < artifact_count && !conflict; i++)
        conflict = path_identity_equal(path, artifacts[i]) != 0;
    if (!conflict) (void) remove(path);
    free(path);
}

static int output_conflicts_with_source(const char *source_file, const char *output_file) {
    return path_identity_equal(source_file, output_file) != 0;
}

static int artifact_conflicts_with_program(const AstProgram *program, const char *path) {
    if (output_conflicts_with_source(program->source_path, path)) return 1;
    for (size_t i = 0; i < program->loaded_source_count; i++)
        if (output_conflicts_with_source(program->loaded_source_paths[i], path)) return 1;
    for (size_t i = 0; i < program->owned_import_count; i++)
        if (artifact_conflicts_with_program(program->owned_imports[i], path)) return 1;
    return 0;
}

static int dump_file(const char *path, int (*writer)(FILE *, const void *),
                     const void *value) {
    FILE *output = fopen(path, "w");
    if (output == NULL) return 0;
    errno = 0;
    int success = writer(output, value);
    if (fclose(output) != 0) success = 0;
    int saved_errno = errno;
    if (!success) (void) remove(path);
    errno = saved_errno;
    return success;
}

static int write_ast_dump(FILE *output, const void *value) {
    return ast_dump(output, value);
}

static int write_ir_dump(FILE *output, const void *value) {
    return ir_dump(output, value);
}

static int write_token_dump(FILE *output, const void *value) {
    return frontend_dump_tokens(output, value);
}

static int write_symbol_dump(FILE *output, const void *value) {
    return semantic_dump(output, value);
}

static int write_cfg_dump(FILE *output, const void *value) {
    return ir_cfg_dump(output, value);
}

typedef struct {
    IrModule *module;
    IrOptimizationStats *stats;
} IrOptimizationTrace;

static int write_ir_optimization_trace(FILE *output, const void *value) {
    const IrOptimizationTrace *trace = value;
    return ir_optimize_module_traced(trace->module, trace->stats, output);
}

int main(int argc, char *argv[]) {
    BackendEmission emission = BACKEND_EXECUTABLE;
    LinkMode link_mode = LINK_AUTO;
    const char *linker_driver = NULL, *runtime_shim = NULL;
    const char *native_libraries[argc > 0 ? (size_t) argc : 1];
    const char *native_directories[argc > 0 ? (size_t) argc : 1];
    NativeLinkOptions native_options = {.libraries = native_libraries, .directories = native_directories};
    const char *native_link_dump = NULL;
    int emission_requested = 0;
    int runtime_component = 0;
    int optimize = 1;
    int optimization_requested = 0;
    int show_tokens = 0;
    int debug_mode = 0;
    int format_error = 0;
    int deterministic = 0;
    int ide_mode = 0;
    const char *ide_buffer = NULL;
    const char *source_file = NULL;
    const char *requested_output = NULL;
    const char *ast_dump_path = NULL;
    const char *token_dump_path = NULL;
    const char *symbol_dump_path = NULL;
    const char *preopt_ir_dump_path = NULL;
    const char *ir_pass_dump_path = NULL;
    const char *ir_dump_path = NULL;
    const char *cfg_dump_path = NULL;
    const char *source_map_path = NULL;
    SyntaxMode syntax_mode = SYNTAX_INTEL; /* Default to Intel syntax */
    TargetFormat target_format = TARGET_ELF; /* Auto-detect later */
    int target_format_explicit = 0; /* Whether user specified target */

    ErrorHandler *error_handler = error_handler_init();
    if (error_handler) {
        error_handler_set_global(error_handler);
    }

    /* Select the diagnostic format before reporting any option error. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--formatError") == 0) {
            format_error = 1;
            if (error_handler) error_handler_set_json_output(error_handler, 1);
        }
    }

    int command_index = 1;
    while (command_index < argc && !strcmp(argv[command_index], "--formatError")) command_index++;
    if (command_index < argc && !strcmp(argv[command_index], "manifest")) {
        const char *action = NULL, *directory = NULL;
        int invalid = 0;
        for (int i = command_index + 1; i < argc; i++) {
            if (!strcmp(argv[i], "--formatError")) continue;
            if (!action) action = argv[i];
            else if (!directory && argv[i][0] != '-') directory = argv[i];
            else invalid = 1;
        }
        if (invalid || !action || strcmp(action, "sync")) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INVALID_OPTION, NULL, "Expected 'manifest sync [package_directory] [--formatError]'");
            error_handler_flush(error_handler);
            error_handler_free(error_handler);
            return 1;
        }
        int ok = frontend_sync_manifest(directory ? directory : ".");
        if (!ok && !error_handler_get_error_count(error_handler))
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INTERNAL_FAILURE, directory, "Manifest command failed");
        error_handler_flush(error_handler);
        error_handler_free(error_handler);
        if (ok) printf("Manifest %s complete\n", action);
        return ok ? 0 : 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            show_tokens = 1;
        } else if (strcmp(argv[i], "--ide") == 0) {
            ide_mode = 1;
        } else if (strcmp(argv[i], "--ide-buffer") == 0) {
            if (++i >= argc) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Option '--ide-buffer' requires a filename");
                error_handler_flush(error_handler);
                error_handler_free(error_handler);
                return 1;
            }
            ide_buffer = argv[i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--formatError") == 0) {
            /* Handled by the pre-scan above. */
        } else if (strcmp(argv[i], "--deterministic") == 0) {
            deterministic = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Option '-o' requires a filename");
                error_handler_flush(error_handler);
                error_handler_free(error_handler);
                return 1;
            }
            requested_output = argv[i];
        } else if (strcmp(argv[i], "--dump-ast") == 0 ||
                   strcmp(argv[i], "--dump-tokens") == 0 ||
                   strcmp(argv[i], "--dump-symbols") == 0 ||
                   strcmp(argv[i], "--dump-ir-before-opt") == 0 ||
                   strcmp(argv[i], "--dump-ir-passes") == 0 ||
                   strcmp(argv[i], "--dump-ir") == 0 ||
                   strcmp(argv[i], "--dump-cfg") == 0 ||
                   strcmp(argv[i], "--source-map") == 0) {
            const char *option = argv[i];
            if (++i >= argc) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Option '%s' requires a filename", option);
                error_handler_flush(error_handler);
                error_handler_free(error_handler);
                return 1;
            }
            if (strcmp(option, "--dump-ast") == 0) ast_dump_path = argv[i];
            else if (strcmp(option, "--dump-tokens") == 0) token_dump_path = argv[i];
            else if (strcmp(option, "--dump-symbols") == 0) symbol_dump_path = argv[i];
            else if (strcmp(option, "--dump-ir-before-opt") == 0) preopt_ir_dump_path = argv[i];
            else if (strcmp(option, "--dump-ir-passes") == 0) ir_pass_dump_path = argv[i];
            else if (strcmp(option, "--dump-ir") == 0) ir_dump_path = argv[i];
            else if (strcmp(option, "--dump-cfg") == 0) cfg_dump_path = argv[i];
            else source_map_path = argv[i];
        } else if (!strcmp(argv[i], "-O0") || !strcmp(argv[i], "-O1")) {
            optimize = !strcmp(argv[i], "-O1");
            optimization_requested = 1;
        } else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-S")) {
            emission = !strcmp(argv[i], "-c") ? BACKEND_OBJECT : BACKEND_ASSEMBLY;
            emission_requested = 1;
        } else if (!strcmp(argv[i], "--runtime-component")) {
            runtime_component = 1;
            if (!emission_requested) emission = BACKEND_OBJECT;
        } else if (strncmp(argv[i], "--emit=", 7) == 0) {
            emission_requested = 1;
            const char *mode = argv[i] + 7;
            if (!strcmp(mode, "asm")) emission = BACKEND_ASSEMBLY;
            else if (!strcmp(mode, "obj")) emission = BACKEND_OBJECT;
            else if (!strcmp(mode, "exe")) emission = BACKEND_EXECUTABLE;
            else {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Invalid emission mode: %s (use 'asm', 'obj' or 'exe')",
                             mode);
                format_error = 1;
            }
        } else if (strncmp(argv[i], "--link=", 7) == 0) {
            const char *mode = argv[i] + 7;
            if (!strcmp(mode, "auto")) link_mode = LINK_AUTO;
            else if (!strcmp(mode, "internal")) link_mode = LINK_INTERNAL;
            else if (!strcmp(mode, "external")) link_mode = LINK_EXTERNAL;
            else
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Invalid link mode: %s", mode);
        } else if (!strcmp(argv[i], "--native-library") || !strcmp(argv[i], "--native-library-dir") ||
                   !strcmp(argv[i], "--dump-native-link")) {
            const char *option = argv[i];
            if (++i >= argc || !argv[i][0]) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Missing value for %s", option);
                break;
            }
            if (!strcmp(option, "--native-library")) native_libraries[native_options.library_count++] = argv[i];
            else if (!strcmp(option, "--native-library-dir"))
                native_directories[native_options.directory_count++] = argv[i];
            else native_link_dump = argv[i];
            if (!driver_native_options_valid(&native_options))
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL,
                             "Native library overrides require unique logical NAME=PATH entries");
        } else if (!strcmp(argv[i], "--linker-driver") || !strcmp(argv[i], "--runtime-shim")) {
            const char *option = argv[i];
            if (++i >= argc || !argv[i][0]) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Missing value for %s", option);
                break;
            }
            if (!strcmp(option, "--linker-driver")) linker_driver = argv[i];
            else runtime_shim = argv[i];
        } else if (strncmp(argv[i], "--syntax=", 9) == 0) {
            const char *mode = argv[i] + 9;
            if (strcmp(mode, "intel") == 0) {
                syntax_mode = SYNTAX_INTEL;
            } else if (strcmp(mode, "att") == 0) {
                syntax_mode = SYNTAX_ATT;
            } else {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL,
                             "Invalid syntax mode: %s (use 'intel' or 'att')", mode);
                error_handler_flush(error_handler);
                print_usage(argv[0]);
                error_handler_free(error_handler);
                return 1;
            }
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            const char *fmt = argv[i] + 9;
            if (strcmp(fmt, "elf") == 0) {
                target_format = TARGET_ELF;
                target_format_explicit = 1;
            } else if (strcmp(fmt, "coff") == 0) {
                target_format = TARGET_COFF;
                target_format_explicit = 1;
            } else {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL,
                             "Invalid target format: %s (use 'elf' or 'coff')", fmt);
                error_handler_flush(error_handler);
                print_usage(argv[0]);
                error_handler_free(error_handler);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            error_handler_free(error_handler);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("DMM Compiler %s\n", DMM_VERSION);
            error_handler_free(error_handler);
            return 0;
        } else if (argv[i][0] != '-') {
            if (source_file != NULL) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL, "Multiple source files are not supported");
                error_handler_flush(error_handler);
                error_handler_free(error_handler);
                return 1;
            }
            source_file = argv[i];
        } else {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INVALID_OPTION, NULL,
                         "Unknown option: %s", argv[i]);
            error_handler_flush(error_handler);
            print_usage(argv[0]);
            error_handler_free(error_handler);
            return 1;
        }
    }

    /* Auto-detect target format if not explicitly set */
    if (!target_format_explicit) {
#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
        target_format = TARGET_COFF;
#else
        target_format = TARGET_ELF;
#endif
    }

    if (!source_file) {
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_NO_SOURCE_FILE, NULL,
                     "No source file specified");
        error_handler_flush(error_handler);
        print_usage(argv[0]);
        error_handler_free(error_handler);
        return 1;
    }

    if (runtime_component && (emission == BACKEND_EXECUTABLE || link_mode != LINK_AUTO ||
                              linker_driver || runtime_shim)) {
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INVALID_OPTION, source_file,
                     "Runtime components require object/assembly emission without linker or runtime overrides");
        error_handler_flush(error_handler);
        error_handler_free(error_handler);
        return 1;
    }
    if (ir_pass_dump_path != NULL && !optimize) {
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INVALID_OPTION, source_file,
                     "--dump-ir-passes requires IR optimization; remove -O0 or use -O1");
        error_handler_flush(error_handler);
        error_handler_free(error_handler);
        return 1;
    }

    if (debug_mode || show_tokens) {
        print_header(source_file);

        printf("================================================================\n");
        printf("           PHASE 1: LEXICAL ANALYSIS (LEXER)\n");
        printf("================================================================\n");
        printf("\n");
    }

    if ((ide_buffer != NULL && !ide_mode) || (
            ide_mode && (ast_dump_path == NULL || requested_output != NULL || token_dump_path != NULL ||
                         symbol_dump_path != NULL || preopt_ir_dump_path != NULL || ir_pass_dump_path != NULL ||
                         ir_dump_path != NULL || cfg_dump_path != NULL || source_map_path != NULL || native_link_dump !=
                         NULL || debug_mode || show_tokens ||
                         optimization_requested || (
                             emission_requested && emission != BACKEND_ASSEMBLY) ||
                         output_conflicts_with_source(source_file, ast_dump_path) ||
                         (ide_buffer != NULL && output_conflicts_with_source(ide_buffer, ast_dump_path))))) {
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INVALID_OPTION, source_file,
                     "--ide requires a distinct --dump-ast path and does not accept output/debug options");
        error_handler_flush(error_handler);
        error_handler_free(error_handler);
        return 1;
    }
    /* Editor analysis must not remove or generate assembly artifacts. */

    FrontendOptions frontend_options = {
        .debug = debug_mode,
        .show_tokens = show_tokens,
        .recover_syntax = ide_mode,
        .has_target = 1,
        .target_format = target_format,
        .system_packages = !runtime_component
    };
    char *override_name = NULL;
    if (ide_buffer != NULL) {
#ifdef _WIN32
        override_name = _fullpath(NULL, source_file, 0);
#else
        override_name = realpath(source_file, NULL);
#endif
        if (override_name != NULL)
            for (char *p = override_name; *p != '\0'; p++) if (*p == '\\') *p = '/';
        error_handler->source_override_name = override_name == NULL ? source_file : override_name;
        error_handler->source_override_path = ide_buffer;
    }
    AstProgram *program = frontend_parse_file(source_file, &frontend_options);
    if (program) {
        program->executable_build = !ide_mode && emission == BACKEND_EXECUTABLE;
        program->runtime_component = runtime_component;
    }
    if (program == NULL) {
        error_handler_flush(error_handler);
        error_handler_free(error_handler);
        free(override_name);
        return 1;
    }
    const char *requested_artifacts[] = {
        requested_output, ast_dump_path, token_dump_path, symbol_dump_path, preopt_ir_dump_path,
        ir_pass_dump_path, ir_dump_path, cfg_dump_path, source_map_path, native_link_dump
    };
    for (size_t i = 0; i < sizeof(requested_artifacts) / sizeof(requested_artifacts[0]); i++) {
        if (driver_native_input_conflicts(&native_options, requested_artifacts[i]) ||
            (emission == BACKEND_EXECUTABLE && link_mode != LINK_INTERNAL &&
             driver_link_input_conflicts(target_format, requested_artifacts[i], linker_driver, runtime_shim))) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INVALID_OPTION, source_file, "Generated artifact conflicts with linker input");
            error_handler_flush(error_handler);
            ast_program_free(program);
            error_handler_free(error_handler);
            free(override_name);
            return 1;
        }
        if (artifact_conflicts_with_program(program, requested_artifacts[i])) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INVALID_OPTION, source_file,
                         "Generated artifact '%s' must differ from every loaded source file", requested_artifacts[i]);
            error_handler_flush(error_handler);
            ast_program_free(program);
            error_handler_free(error_handler);
            free(override_name);
            return 1;
        }
        for (size_t j = i + 1; j < sizeof(requested_artifacts) / sizeof(requested_artifacts[0]); j++)
            if (path_identity_equal(requested_artifacts[i], requested_artifacts[j]) != 0) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, source_file, "Generated artifact paths must be distinct");
                error_handler_flush(error_handler);
                ast_program_free(program);
                error_handler_free(error_handler);
                free(override_name);
                return 1;
            }
    }
    if (!ide_mode && emission != BACKEND_EXECUTABLE)
        remove_stale_output(program, source_file, requested_output,
                            requested_artifacts, sizeof(requested_artifacts) / sizeof(requested_artifacts[0]),
                            emission == BACKEND_ASSEMBLY
                                ? ".s"
                                : emission == BACKEND_OBJECT
                                      ? (target_format == TARGET_ELF ? ".o" : ".obj")
                                      : (target_format == TARGET_ELF ? ".out" : ".exe"));
    if (ide_mode) {
        SemanticModel *semantics = NULL;
        if (ast_validate_program(program)) {
            semantics = semantic_analyze_target(program, target_format);
            if (semantics == NULL)
                error_report(error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INTERNAL_FAILURE, source_file,
                             "Could not build typed frontend representation for editor analysis");
            else if (!dump_file(ast_dump_path, write_ast_dump, program))
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_DUMP_FAILED, source_file, "Could not write editor AST information to '%s': %s",
                             ast_dump_path,
                             errno == 0 ? "internal serialization failure" : strerror(errno));
        } else if (error_handler_get_error_count(error_handler) == 0) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_PARSER,
                         ERR_PARSE_INVALID_SYNTAX, source_file, "Could not construct a safe editor syntax tree");
        }
        int failed = error_handler_get_error_count(error_handler) != 0;
        error_handler_flush(error_handler);
        semantic_model_free(semantics);
        ast_program_free(program);
        error_handler_free(error_handler);
        free(override_name);
        return failed;
    }
    if (token_dump_path != NULL && !dump_file(token_dump_path, write_token_dump, program)) {
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_DUMP_FAILED, source_file, "Could not write token dump '%s': %s", token_dump_path,
                     errno == 0 ? "internal serialization failure" : strerror(errno));
        error_handler_flush(error_handler);
        ast_program_free(program);
        error_handler_free(error_handler);
        free(override_name);
        return 1;
    }
    if (!program->structured_ast_complete) {
        const AstToken *token = ast_program_token(program, program->structured_error_token);
        if (error_handler_get_error_count(error_handler) == 0)
            error_report(error_handler, SEVERITY_ERROR,
                         token == NULL ? 0 : token->span.begin.line,
                         token == NULL ? 0 : token->span.begin.column,
                         ERROR_CATEGORY_PARSER, ERR_PARSE_INVALID_SYNTAX, source_file,
                         "Could not construct a complete syntax tree");
        error_handler_flush(error_handler);
        ast_program_free(program);
        error_handler_free(error_handler);
        return 1;
    }
    if (error_handler_get_error_count(error_handler) != 0) {
        error_handler_flush(error_handler);
        ast_program_free(program);
        error_handler_free(error_handler);
        return 1;
    }

    if (output_conflicts_with_source(source_file, requested_output)) {
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INVALID_OPTION, source_file, "Output file must differ from source file");
        error_handler_flush(error_handler);
        ast_program_free(program);
        error_handler_free(error_handler);
        return 1;
    }

    if (debug_mode || show_tokens) {
        printf("  [+] %zu Tokens represented in AST\n", program->token_count);
        printf("  [+] Structured AST: complete (%zu declarations)\n",
               program->structured_declaration_count);
        printf("\n");
    }

    const char *output_suffix = emission == BACKEND_ASSEMBLY
                                    ? ".s"
                                    : emission == BACKEND_OBJECT
                                          ? (target_format == TARGET_ELF ? ".o" : ".obj")
                                          : (target_format == TARGET_ELF ? ".out" : ".exe");
    size_t suffix_length = strlen(output_suffix) + 1;
    char *generated_output = NULL;
    const char *output_filename = requested_output;
    if (output_filename == NULL) {
        size_t source_length = strlen(source_file);
        if (source_length > SIZE_MAX - suffix_length || (generated_output = malloc(source_length + suffix_length)) ==
            NULL) {
            error_report(error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INTERNAL_FAILURE, source_file, "Out of memory while creating output path");
            error_handler_flush(error_handler);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        memcpy(generated_output, source_file, source_length);
        memcpy(generated_output + source_length, output_suffix, suffix_length);
        output_filename = generated_output;
    }
    const char *artifacts[] = {
        output_filename, ast_dump_path, token_dump_path, symbol_dump_path, preopt_ir_dump_path,
        ir_pass_dump_path, ir_dump_path, cfg_dump_path, source_map_path, native_link_dump
    };
    for (size_t i = 0; i < sizeof(artifacts) / sizeof(artifacts[0]); i++) {
        if (driver_native_input_conflicts(&native_options, artifacts[i]) ||
            (emission == BACKEND_EXECUTABLE && link_mode != LINK_INTERNAL &&
             driver_link_input_conflicts(target_format, artifacts[i], linker_driver, runtime_shim))) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INVALID_OPTION, source_file, "Generated artifact conflicts with linker input");
            error_handler_flush(error_handler);
            free(generated_output);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        if (artifacts[i] != NULL && artifact_conflicts_with_program(program, artifacts[i])) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INVALID_OPTION, source_file,
                         "Generated artifact must differ from source file");
            error_handler_flush(error_handler);
            free(generated_output);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        for (size_t j = i + 1; j < sizeof(artifacts) / sizeof(artifacts[0]); j++) {
            if (artifacts[i] != NULL && artifacts[j] != NULL &&
                path_identity_equal(artifacts[i], artifacts[j]) != 0) {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, source_file,
                             "Generated artifact paths must be distinct");
                error_handler_flush(error_handler);
                free(generated_output);
                ast_program_free(program);
                error_handler_free(error_handler);
                return 1;
            }
        }
    }
    if (requested_output != NULL && emission != BACKEND_EXECUTABLE)
        (void) remove(requested_output);

    if (debug_mode) {
        printf("\n================================================================\n");
        printf("           PHASE 3: AST LOWERING AND CODE GENERATION\n");
        printf("================================================================\n\n");
    }

    BackendOptions backend_options = {
        .target_format = target_format,
        .syntax_mode = syntax_mode,
        .debug = debug_mode,
        .deterministic = deterministic,
        .emission = emission,
        .source_map_path = source_map_path
    };
    SemanticModel *semantics = NULL;
    IrModule *module = NULL;
    if (program->structured_ast_complete) {
        semantics = semantic_analyze_target(program, target_format);
        if (semantics == NULL) {
            error_report(error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INTERNAL_FAILURE, source_file,
                         "Could not build typed frontend representation");
            error_handler_flush(error_handler);
            free(generated_output);
            ir_module_free(module);
            semantic_model_free(semantics);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        const char *failed_dump = NULL;
        if (ast_dump_path != NULL && !dump_file(ast_dump_path, write_ast_dump, program))
            failed_dump = ast_dump_path;
        else if (symbol_dump_path != NULL && !dump_file(symbol_dump_path, write_symbol_dump, semantics))
            failed_dump = symbol_dump_path;
        if (failed_dump != NULL) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_DUMP_FAILED, source_file,
                         "Could not write requested frontend dump '%s': %s", failed_dump,
                         errno == 0 ? "internal serialization failure" : strerror(errno));
            error_handler_flush(error_handler);
            free(generated_output);
            semantic_model_free(semantics);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        if (semantics->error_count != 0) {
            error_handler_flush(error_handler);
            free(generated_output);
            semantic_model_free(semantics);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        AstOptimizationStats ast_stats = {0};
        if (optimize) ast_optimize_program(program, &ast_stats);
        if (debug_mode && optimize)
            printf(
                "  [+] AST optimization: %zu branches, %zu constant loops, %zu short circuits, %zu unreachable statements\n",
                ast_stats.constant_branches, ast_stats.constant_loops, ast_stats.short_circuits,
                ast_stats.unreachable_statements);
        module = ir_lower_program(program, semantics);
        if (module == NULL) {
            if (error_handler_get_error_count(error_handler) == 0)
                error_report(error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INTERNAL_FAILURE, source_file,
                             "Could not create lowering module");
            error_handler_flush(error_handler);
            free(generated_output);
            semantic_model_free(semantics);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        if (preopt_ir_dump_path != NULL && !dump_file(preopt_ir_dump_path, write_ir_dump, module))
            failed_dump = preopt_ir_dump_path;
        if (failed_dump != NULL) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_DUMP_FAILED, source_file,
                         "Could not write requested frontend dump '%s': %s", failed_dump,
                         errno == 0 ? "internal serialization failure" : strerror(errno));
            error_handler_flush(error_handler);
            free(generated_output);
            ir_module_free(module);
            semantic_model_free(semantics);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        IrOptimizationStats optimization_stats = {0};
        int optimization_ok = 1;
        if (optimize) {
            if (ir_pass_dump_path != NULL) {
                IrOptimizationTrace trace = {
                    .module = module,
                    .stats = &optimization_stats
                };
                optimization_ok = dump_file(ir_pass_dump_path, write_ir_optimization_trace, &trace);
            } else {
                optimization_ok = ir_optimize_module(module, &optimization_stats);
            }
        }
        if (!optimization_ok) {
            error_report(error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INTERNAL_FAILURE, source_file,
                         ir_pass_dump_path != NULL
                             ? "IR optimization, verification, or pass trace dump failed"
                             : "IR optimization or verification failed");
            error_handler_flush(error_handler);
            free(generated_output);
            ir_module_free(module);
            semantic_model_free(semantics);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        if (debug_mode && optimize) {
            printf(
                "  [+] IR optimization: %zu folds, %zu constants, %zu copies, %zu common expressions, %zu reused bounds checks, %zu loop invariants, %zu dead values, %zu dead stores, %zu dead functions, %zu branches, %zu threaded jumps, %zu blocks, %zu addresses\n",
                optimization_stats.constants_folded, optimization_stats.constants_propagated,
                optimization_stats.copies_propagated, optimization_stats.common_expressions,
                optimization_stats.bounds_checks_reused, optimization_stats.loop_invariants_hoisted,
                optimization_stats.dead_instructions, optimization_stats.dead_stores,
                optimization_stats.dead_functions, optimization_stats.branches_folded,
                optimization_stats.jumps_threaded,
                optimization_stats.blocks_removed, optimization_stats.addresses_simplified);
        }
        if (ir_dump_path != NULL && !dump_file(ir_dump_path, write_ir_dump, module)) failed_dump = ir_dump_path;
        else if (cfg_dump_path != NULL && !dump_file(cfg_dump_path, write_cfg_dump, module))
            failed_dump = cfg_dump_path;
        if (failed_dump != NULL) {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_DUMP_FAILED, source_file,
                         "Could not write requested frontend dump '%s': %s", failed_dump,
                         errno == 0 ? "internal serialization failure" : strerror(errno));
            error_handler_flush(error_handler);
            free(generated_output);
            ir_module_free(module);
            semantic_model_free(semantics);
            ast_program_free(program);
            error_handler_free(error_handler);
            return 1;
        }
        if (debug_mode) {
            printf("  [+] Semantic symbols: %zu\n", semantics->symbol_count);
            printf("  [+] Verified typed IR functions: %zu\n", module->function_count);
        }
    }
    /* CLI compatibility is translated to a requirement once. Runtime and backend
       only receive the resolved profile, never the requested link strategy. */
    if (link_mode == LINK_EXTERNAL) module->runtime_requirements |= RUNTIME_REQUIRE_PLATFORM;
    ir_select_runtime_functions(module);
    RuntimeRequirements requirements = ir_runtime_requirements(module);
    int uses_native = 0;
    for (size_t n = 0; n < module->native_import_count; ++n)
        /* The private primitive ABI is defined in the emitted runtime itself.
           Ordinary FFI declarations for it do not introduce an OS library. */
        if (strcmp(module->native_imports[n].library, "dmm_runtime"))
            uses_native |= ir_native_import_used(module, module->native_imports[n].symbol_id);
    if (uses_native) requirements |= RUNTIME_REQUIRE_PLATFORM;
    for (size_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].is_native_export &&
            (!module->emission_selected || module->functions[f].emission_reachable))
            requirements |= RUNTIME_REQUIRE_PLATFORM;
    if (link_mode == LINK_EXTERNAL) requirements |= RUNTIME_REQUIRE_PLATFORM;
    backend_options.runtime_profile = runtime_profile_for(requirements);
    LinkMode resolved_link = LINK_INTERNAL;
    int selection_ok = runtime_resolve_link(link_mode, backend_options.runtime_profile, &resolved_link);
    if (!selection_ok)
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INVALID_OPTION, source_file, uses_native
                                                               ? "Internal linking cannot resolve native imports; use --link=auto or --link=external with GCC/Clang"
                                                               : "Internal linking cannot satisfy platform runtime requirements");
    if (emission == BACKEND_EXECUTABLE && resolved_link != LINK_EXTERNAL && (linker_driver || runtime_shim)) {
        selection_ok = 0;
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INVALID_OPTION, source_file,
                     "Linker driver and runtime shim require external executable linking");
    }
    /* AUTO can acquire platform requirements during IR lowering. Preserve an
       existing external executable until the isolated link publishes success. */
    if (selection_ok && requested_output && emission == BACKEND_EXECUTABLE && resolved_link == LINK_INTERNAL)
        (void) remove(requested_output);
    if (selection_ok && native_link_dump &&
        !driver_dump_native_link(module, &backend_options, &native_options, native_link_dump)) {
        selection_ok = 0;
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_DUMP_FAILED, source_file, "Could not write native link requirements: %s",
                     native_link_dump);
    }
    int emitted = selection_ok && (emission == BACKEND_EXECUTABLE && resolved_link == LINK_EXTERNAL
                                       ? driver_external_link(module, &backend_options, output_filename, linker_driver,
                                                              runtime_shim, &native_options)
                                       : backend_emit_file(module, &backend_options, output_filename));
    if (!emitted) {
        error_handler_flush(error_handler);
        if (!format_error) fprintf(stderr, "\n[ERROR] Compilation failed!\n\n");
        free(generated_output);
        ir_module_free(module);
        semantic_model_free(semantics);
        ast_program_free(program);
        error_handler_free(error_handler);
        return 1;
    }
    if (debug_mode) {
        printf("\n");
        printf("################################################################\n");
        printf("#                                                              #\n");
        printf("#            [SUCCESS] COMPILATION COMPLETED!                 #\n");
        printf("#                                                              #\n");
        printf("################################################################\n");
        printf("\n");
    }

    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    free(generated_output);

    if (error_handler) {
        error_handler_flush(error_handler);
        error_handler_free(error_handler);
    }

    return 0;
}
