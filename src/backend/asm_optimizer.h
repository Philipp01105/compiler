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

#endif /* ASM_OPTIMIZER_H */
