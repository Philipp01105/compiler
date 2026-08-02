#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "external_link.h"
#include "errorHandler.h"
#include "path_identity.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

static int failure(const IrModule *module, const char *message, const char *detail) {
    error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                 ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path, "%s: %s", message, detail);
    return 0;
}

static char *default_shim(TargetFormat target) {
    char executable[4096];
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, executable, sizeof(executable));
    if (!length || length >= sizeof(executable)) return NULL;
#else
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0) return NULL;
    executable[length] = '\0';
#endif
    char *separator = strrchr(executable, '/');
    char *backslash = strrchr(executable, '\\');
    if (backslash && (!separator || backslash > separator)) separator = backslash;
    if (!separator) return NULL;
    *separator = '\0';
    size_t capacity = strlen(executable) + 64;
    char *path = malloc(capacity);
    if (path) snprintf(path, capacity, "%s/dmm-runtime/%s/platform-shim.o", executable,
                       target == TARGET_ELF ? "elf" : "coff");
    return path;
}

static int shim_matches(const char *path, TargetFormat target) {
    unsigned char magic[20];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t count = fread(magic, 1, sizeof(magic), file);
    fclose(file);
    if (count != sizeof(magic)) return 0;
    if (target == TARGET_COFF) return magic[0] == 0x64 && magic[1] == 0x86;
    return !memcmp(magic, "\177ELF", 4) && magic[4] == 2 && magic[5] == 1 &&
           magic[16] == 1 && magic[17] == 0 && magic[18] == 62 && magic[19] == 0;
}

int driver_link_input_conflicts(TargetFormat target, const char *artifact,
                                const char *linker_driver, const char *runtime_shim) {
    if (!artifact) return 0;
    char *owned = runtime_shim ? NULL : default_shim(target);
    const char *shim = runtime_shim ? runtime_shim : owned;
    int conflict = (shim && path_identity_equal(artifact, shim) != 0) ||
                   (linker_driver && path_identity_equal(artifact, linker_driver) != 0);
    free(owned);
    return conflict;
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
                         const char *linker_driver, const char *runtime_shim) {
#ifdef _WIN32
    TargetFormat host = TARGET_COFF;
#else
    TargetFormat host = TARGET_ELF;
#endif
    if (options->target_format != host && (!linker_driver || !runtime_shim))
        return failure(module, "Cross-linking requires --linker-driver and --runtime-shim", output);
    char *owned_shim = runtime_shim ? NULL : default_shim(options->target_format);
    const char *shim = runtime_shim ? runtime_shim : owned_shim;
    if (!shim || !shim_matches(shim, options->target_format)) {
        int result = failure(module, "Missing or incompatible private runtime shim", shim ? shim : "unknown compiler location");
        free(owned_shim);
        return result;
    }
    if (path_identity_equal(output, shim) != 0 ||
        (options->source_map_path && path_identity_equal(options->source_map_path, shim) != 0)) {
        free(owned_shim);
        return failure(module, "Generated artifact conflicts with runtime shim", output);
    }
    if (ir_runtime_requirements(module) & RUNTIME_REQUIRE_NETWORK) {
        free(owned_shim);
        return failure(module, "NETWORK runtime support is not implemented", output);
    }
    size_t capacity = strlen(output) + 128;
    char *directory = malloc(capacity);
    char *object = malloc(capacity);
    char *image = malloc(capacity);
    char *log = malloc(capacity);
    if (!directory || !object || !image || !log) {
        free(directory); free(object); free(image); free(log); free(owned_shim);
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
    const char *arguments[] = {
        linker_driver ? linker_driver : "gcc", object, shim, "-o", image,
        options->target_format == TARGET_ELF ? "-no-pie" : "-Wl,--subsystem,console",
        options->target_format == TARGET_ELF ? "-pthread" : "-lkernel32", NULL
    };
    unsigned long code = 0;
    if (!run_process(arguments, log, &code) || code) {
        char details[32768];
        size_t count = 0;
        FILE *file = fopen(log, "r");
        if (file) { count = fread(details, 1, sizeof(details) - 1, file); fclose(file); }
        details[count] = '\0';
        char heading[256];
        snprintf(heading, sizeof(heading), "External linker '%s' failed (status %lu)", arguments[0], code);
        failure(module, heading, count ? details : "process could not start or produced no diagnostics");
        goto cleanup;
    }
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
    free(directory); free(object); free(image); free(log); free(owned_shim);
    return success;
}
