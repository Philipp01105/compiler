#ifndef ASM_OPTIMIZER_H
#define ASM_OPTIMIZER_H

/*
 * Assembly Optimizer
 * 
 * This module provides post-processing optimization for generated assembly code.
 * It removes dead/unreachable code and performs other optimizations to improve
 * code quality and reduce binary size.
 */

/*
 * cleanup_assembly_file - Remove unreachable code from generated assembly
 * @filename: Path to the assembly file to clean up
 *
 * Returns: 0 on success, -1 on error
 */
int cleanup_assembly_file(const char *filename);

#endif /* ASM_OPTIMIZER_H */
