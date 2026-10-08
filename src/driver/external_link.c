#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "external_link.h"
#include "errorHandler.h"
#include "path_identity.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#ifdef _WIN32
#define TokenType WindowsTokenType
#include <windows.h>
#undef TokenType
#include <direct.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

static const char *native_override(const NativeLinkOptions *options, const char *name) {
    for (size_t i = 0; i < options->library_count; ++i) {
        const char *entry = options->libraries[i];
        const char *equal = strchr(entry, '=');
        if (equal && (size_t)(equal - entry) == strlen(name) && !strncmp(entry, name, strlen(name))) return equal + 1;
    }
    return NULL;
}

int driver_native_options_valid(const NativeLinkOptions *options) {
    for (size_t i = 0; i < options->library_count; ++i) {
        const char *entry = options->libraries[i], *equal = strchr(entry, '=');
        if (!equal || equal == entry || !equal[1]) return 0;
        for (const char *p = entry; p < equal; ++p)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' ||
                  (p != entry && ((*p >= '0' && *p <= '9') || *p == '-')))) return 0;
        for (size_t j = 0; j < i; ++j) {
            const char *previous = options->libraries[j];
            const char *separator = strchr(previous, '=');
            if (separator && separator - previous == equal - entry &&
                !strncmp(previous, entry, (size_t)(equal - entry))) return 0;
        }
    }
    return 1;
}

int driver_native_input_conflicts(const NativeLinkOptions *options, const char *artifact) {
    if (!artifact) return 0;
    for (size_t i = 0; i < options->library_count; ++i) {
        const char *equal = strchr(options->libraries[i], '=');
        if (equal && path_identity_equal(artifact, equal + 1) != 0) return 1;
    }
    return 0;
}

static void link_quote(FILE *file, const char *text) {
    fputc('"', file);
    for (; *text; ++text) {
        if (*text == '"' || *text == '\\') fputc('\\', file);
        if (*text == '\n') fputs("\\n", file);
        else if (*text == '\r') fputs("\\r", file);
        else fputc(*text, file);
    }
    fputc('"', file);
}

int driver_dump_native_link(const IrModule *module, const BackendOptions *options,
                            const NativeLinkOptions *native, const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) return 0;
    fprintf(file, "dmm-native-link-v2\ntarget=%s\nruntime-profile=%s\n",
            options->target_format == TARGET_ELF ? "elf-x86_64-system-v" : "coff-x86_64-mingw-ucrt",
            module->program->runtime_component ? "component" :
            options->runtime_profile == RUNTIME_PLATFORM ? "platform" : "standalone");
    for (size_t i = 0; i < native->directory_count; ++i) {
        fputs("library-directory=", file); link_quote(file, native->directories[i]); fputc('\n', file);
    }
    for (size_t i = 0; i < module->native_import_count; ++i) {
        const IrNativeImport *import = &module->native_imports[i];
        if (!ir_native_import_used(module, import->symbol_id)) continue;
        fputs("import library=", file); link_quote(file, import->library);
        fputs(" symbol=", file); link_quote(file, import->native_name);
        const char *override = native_override(native, import->library);
        if (override) {fputs(" path=", file); link_quote(file, override);}
        fputc('\n', file);
    }
    int success = !ferror(file);
    if (fclose(file)) success = 0;
    return success;
}

static int failure(const IrModule *module, const char *message, const char *detail) {
    error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                 ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path, "%s: %s", message, detail);
    return 0;
}

static int object_magic_matches(const unsigned char *magic, TargetFormat target) {
    if (target == TARGET_COFF) return magic[0] == 0x64 && magic[1] == 0x86;
    return !memcmp(magic, "\177ELF", 4) && magic[4] == 2 && magic[5] == 1 &&
           magic[16] == 1 && magic[17] == 0 && magic[18] == 62 && magic[19] == 0;
}

static int shim_matches(const char *path, TargetFormat target) {
    unsigned char magic[20];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t count = fread(magic, 1, sizeof(magic), file);
    int valid = count == sizeof(magic) && object_magic_matches(magic, target);
    if (count >= 8 && !memcmp(magic, "!<arch>\n", 8)) {
        /* Validate every object member; GNU/LLVM archive metadata is target-neutral. */
        valid = fseek(file, 0, SEEK_END) == 0;
        long archive_size = valid ? ftell(file) : -1;
        valid = archive_size >= 8 && fseek(file, 8, SEEK_SET) == 0;
        size_t objects = 0;
        unsigned char header[60];
        while (valid) {
            size_t read = fread(header, 1, sizeof(header), file);
            if (!read) { valid = !ferror(file); break; }
            if (read != sizeof(header) || header[58] != '`' || header[59] != '\n') { valid = 0; break; }
            char length[11];
            memcpy(length, header + 48, 10); length[10] = '\0';
            char *end = NULL;
            errno = 0;
            unsigned long bytes = strtoul(length, &end, 10);
            while (end && *end == ' ') ++end;
            if (errno || end == length || !end || *end || bytes > (unsigned long)LONG_MAX - 1) { valid = 0; break; }
            int metadata = header[0] == '/' && (header[1] == ' ' || header[1] == '/' ||
                                               !memcmp(header, "/SYM64/", 7));
            long start = ftell(file);
            if (start < 0 || start > LONG_MAX - (long)bytes - (long)(bytes & 1UL)) { valid = 0; break; }
            if (start + (long)bytes + (long)(bytes & 1UL) > archive_size) { valid = 0; break; }
            if (!metadata) {
                if (bytes < sizeof(magic) || fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
                    !object_magic_matches(magic, target)) { valid = 0; break; }
                ++objects;
            }
            if (fseek(file, start + (long)bytes + (long)(bytes & 1UL), SEEK_SET)) valid = 0;
        }
        valid = valid && objects != 0;
    }
    fclose(file);
    return valid;
}

int driver_link_input_conflicts(TargetFormat target, const char *artifact,
                                const char *linker_driver, const char *runtime_shim) {
    if (!artifact) return 0;
    (void)target;
    return (runtime_shim && path_identity_equal(artifact, runtime_shim) != 0) ||
           (linker_driver && path_identity_equal(artifact, linker_driver) != 0);
}

#ifdef _WIN32
/* Inverse of the Windows CRT argv parser, including trailing backslashes. */
static char *quote_argument(char *out, const char *value) {
    *out++ = '"';
    while (*value) {
        size_t slashes = 0;
        while (*value == '\\') { ++slashes; ++value; }
        size_t repeat = (*value == '"' || !*value) ? slashes * 2 : slashes;
        while (repeat--) *out++ = '\\';
        if (!*value) break;
        if (*value == '"') *out++ = '\\';
        *out++ = *value++;
    }
    *out++ = '"';
    return out;
}
static int run_process(const char *const *arguments, const char *log_path, unsigned long *status) {
    size_t capacity = 1;
    for (size_t i = 0; arguments[i]; ++i) capacity += strlen(arguments[i]) * 2 + 4;
    char *command = malloc(capacity);
    if (!command) return 0;
    char *cursor = command;
    for (size_t i = 0; arguments[i]; ++i) {
        if (i) *cursor++ = ' ';
        cursor = quote_argument(cursor, arguments[i]);
    }
    *cursor = '\0';
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE log = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, &security,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA startup = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = input;
    PROCESS_INFORMATION process = {0};
    int success = log != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE &&
        CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
    if (!success) *status = GetLastError();
    if (success) {
        DWORD code = 0;
        success = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
                  GetExitCodeProcess(process.hProcess, &code);
        *status = code;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    free(command);
    return success;
}
#else
static int run_process(const char *const *arguments, const char *log_path, unsigned long *status) {
    FILE *log = fopen(log_path, "w");
    if (!log) return 0;
    pid_t child = fork();
    if (child == 0) {
        if (dup2(fileno(log), STDOUT_FILENO) < 0 || dup2(fileno(log), STDERR_FILENO) < 0) _exit(126);
        fclose(log);
        execvp(arguments[0], (char *const *)arguments);
        perror(arguments[0]);
        _exit(127);
    }
    fclose(log);
    if (child < 0) return 0;
    int code = 0;
    pid_t result;
    do { result = waitpid(child, &code, 0); } while (result < 0 && errno == EINTR);
    if (result < 0) return 0;
    *status = (unsigned long)(WIFEXITED(code) ? WEXITSTATUS(code) : 128 + WTERMSIG(code));
    return 1;
}
#endif

int driver_external_link(const IrModule *module, const BackendOptions *options,
                         const char *output,
                         const char *linker_driver, const char *runtime_shim,
                         const NativeLinkOptions *native) {
#ifdef _WIN32
    TargetFormat host = TARGET_COFF;
#else
    TargetFormat host = TARGET_ELF;
#endif
    if (options->target_format != host && !linker_driver)
        return failure(module, "Cross-linking requires --linker-driver", output);
    int network=(ir_runtime_requirements(module)&RUNTIME_REQUIRE_NETWORK)!=0;
    // The DMM runtime is compiled into program.o. A supplied shim is an
    // optional additional native object; there is no installed bundle fallback.
    const char *shim = runtime_shim;
    if (shim && !shim_matches(shim, options->target_format))
        return failure(module, "Incompatible additional runtime object", shim);
    if (shim && (path_identity_equal(output, shim) != 0 ||
        (options->source_map_path && path_identity_equal(options->source_map_path, shim) != 0)))
        return failure(module, "Generated artifact conflicts with runtime shim", output);
    size_t capacity = strlen(output) + 128;
    char *directory = malloc(capacity);
    char *object = malloc(capacity);
    char *image = malloc(capacity);
    char *log = malloc(capacity);
    if (!directory || !object || !image || !log) {
        free(directory); free(object); free(image); free(log);
        return failure(module, "External link allocation failed", output);
    }
    int made_directory = 0;
#ifdef _WIN32
    for (unsigned i = 0; i < 100 && !made_directory; ++i) {
        snprintf(directory, capacity, "%s.dmm-link-%lu-%lu-%u", output,
                 (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), i);
        made_directory = CreateDirectoryA(directory, NULL) != 0;
        if (!made_directory && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
#else
    snprintf(directory, capacity, "%s.dmm-link-XXXXXX", output);
    made_directory = mkdtemp(directory) != NULL;
#endif
    int success = 0;
    if (!made_directory) {
        failure(module, "Could not create isolated link directory", output);
        goto done;
    }
    snprintf(object, capacity, "%s/program.o", directory);
    snprintf(image, capacity, "%s/program.exe", directory);
    snprintf(log, capacity, "%s/link.log", directory);
    BackendOptions object_options = *options;
    object_options.emission = BACKEND_OBJECT;
    if (!backend_emit_file(module, &object_options, object)) goto cleanup;
    size_t argument_capacity = 12 + native->directory_count + module->native_import_count;
    const char **arguments = calloc(argument_capacity, sizeof(*arguments));
    char **allocated = calloc(argument_capacity, sizeof(*allocated));
    if (!arguments || !allocated) {free(arguments); free(allocated); goto cleanup;}
    size_t argument_count = 0, owned_count = 0;
    arguments[argument_count++] = linker_driver ? linker_driver : "gcc";
    arguments[argument_count++] = object;
    if (shim) arguments[argument_count++] = shim;
    arguments[argument_count++] = "-o";
    arguments[argument_count++] = image;
    arguments[argument_count++] = options->target_format == TARGET_ELF ? "-no-pie" : "-Wl,--subsystem,console";
    arguments[argument_count++] = options->target_format == TARGET_ELF ? "-pthread" : "-lkernel32";
    if (options->deterministic)
        arguments[argument_count++] = options->target_format == TARGET_COFF
            ? "-Wl,--no-insert-timestamp" : "-Wl,--build-id=none";
    if (network && options->target_format == TARGET_COFF) arguments[argument_count++] = "-lws2_32";
    int libraries_ok = 1;
    for (size_t i = 0; i < native->directory_count; ++i) {
        size_t length = strlen(native->directories[i]) + 3;
        char *argument = malloc(length);
        if (!argument) {libraries_ok = 0; break;}
        snprintf(argument, length, "-L%s", native->directories[i]);
        allocated[owned_count++] = argument;
        arguments[argument_count++] = argument;
    }
    for (size_t i = 0; libraries_ok && i < module->native_import_count; ++i) {
        const IrNativeImport *import = &module->native_imports[i];
        if (!ir_native_import_used(module, import->symbol_id)) continue;
        if (!strcmp(import->library,"dmm_runtime")) continue;
        int duplicate = 0;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(import->library, module->native_imports[j].library) &&
                ir_native_import_used(module, module->native_imports[j].symbol_id)) duplicate = 1;
        if (duplicate) continue;
        const char *override = native_override(native, import->library);
        if (override) {
            FILE *library = fopen(override, "rb");
            if (!library) {failure(module, "Native library override is not readable", override); libraries_ok = 0; break;}
            fclose(library);
            /* Prefix relative paths so filenames beginning with '-' cannot become options. */
            size_t length = strlen(override) + 3;
            char *argument = malloc(length);
            if (!argument) {libraries_ok = 0; break;}
            int absolute = override[0] == '/' || override[0] == '\\' ||
                           (override[0] && override[1] == ':');
            snprintf(argument, length, "%s%s", absolute ? "" : "./", override);
            allocated[owned_count++] = argument;
            arguments[argument_count++] = argument;
        } else {
            size_t length = strlen(import->library) + 3;
            char *argument = malloc(length);
            if (!argument) {libraries_ok = 0; break;}
            snprintf(argument, length, "-l%s", import->library);
            allocated[owned_count++] = argument;
            arguments[argument_count++] = argument;
        }
    }
    unsigned long code = 0;
    int linked = libraries_ok && run_process(arguments, log, &code) && !code;
    if (!linked && libraries_ok) {
        char details[32768];
        size_t count = 0;
        FILE *file = fopen(log, "r");
        if (file) { count = fread(details, 1, sizeof(details) - 1, file); fclose(file); }
        details[count] = '\0';
        char heading[256];
        snprintf(heading, sizeof(heading), "External linker '%s' failed (status %lu)", arguments[0], code);
        failure(module, heading, count ? details : "process could not start or produced no diagnostics");
    }
    for (size_t i = 0; i < owned_count; ++i) free(allocated[i]);
    free(allocated); free(arguments);
    if (!linked) goto cleanup;
#ifdef _WIN32
    success = MoveFileExA(image, output, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    success = rename(image, output) == 0;
#endif
    if (!success) failure(module, "Could not publish linked executable", output);
cleanup:
    (void)remove(object); (void)remove(image); (void)remove(log);
    if (!success && options->source_map_path) (void)remove(options->source_map_path);
#ifdef _WIN32
    (void)_rmdir(directory);
#else
    (void)rmdir(directory);
#endif
done:
    free(directory); free(object); free(image); free(log);
    return success;
}
