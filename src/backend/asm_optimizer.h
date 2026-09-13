#ifndef ASM_OPTIMIZER_H
#define ASM_OPTIMIZER_H

/*
 * Assembly Optimizer
 * 
 * This module provides post-processing optimization for generated assembly code.
 * It performs conservative peephole optimizations on generated assembly.
 */

/*
 * cleanup_assembly_file - Safely optimize a generated assembly file
 * @filename: Path to the assembly file to clean up
 *
 * Returns: 0 on success, -1 on error
 */
int cleanup_assembly_file(const char *filename);

typedef struct {
    const char *operation;
    int error_number;
    unsigned long windows_error;
    char reason[512];
    char path[4096];
} AssemblyCleanupError;

int cleanup_assembly_file_detailed(const char *filename, AssemblyCleanupError *error);

#ifdef DMM_OPTIMIZER_FAULT_TEST
/* Unit-test-only failures; stream close still executes before injection. */
void assembly_cleanup_test_fail(int flush, int close, int replace);
int assembly_cleanup_test_close_count(void);
#endif

#endif /* ASM_OPTIMIZER_H */
