# Compiler Backend Rewrite Plan: COFF, Windows ABI, and Intel Syntax

## Executive Summary

This document outlines the effort required to rewrite the compiler backend to:
1. **Output correct COFF format** (Windows object file format)
2. **Implement proper Windows x64 ABI** (calling conventions)
3. **Switch from AT&T syntax to Intel syntax**

**Estimated Time:** 3-4 weeks for a single experienced developer
**Complexity:** Medium-High
**Risk Level:** Medium (requires careful testing across platforms)

---

## Current State Analysis

### Code Base Metrics
- **Total backend code:** ~6,027 lines across 6 files
- **Assembly generation statements:** ~922 `code_printf` calls
- **Platform-specific code points:** ~15 locations
- **Calling convention dependencies:** ~63 function calls

### Current Architecture

#### Files Requiring Major Changes
1. **src/parser_codegen.c** (164 lines)
   - Syscall generation (Linux-specific)
   - String manipulation helpers
   - Stack management utilities

2. **src/parser_statements.c** (~2000 lines)
   - Statement code generation
   - Variable assignments
   - Control flow (if/else, loops)
   - Function calls and returns

3. **src/parser_expressions.c** (~1500 lines)
   - Expression evaluation
   - Arithmetic operations
   - Function call expressions

4. **src/parser_declarations.c** (~800 lines)
   - Function prologue/epilogue
   - Variable declarations
   - Struct/enum definitions

5. **src/parser_types.c** (208 lines)
   - Calling convention registry
   - Platform detection
   - ABI configuration

6. **src/main.c** (312 lines)
   - Assembly file header generation
   - Platform detection
   - Output formatting

### Existing Windows Support
The compiler has **partial** Windows support:
- Platform detection via `#ifdef _WIN32`
- Windows calling convention (RCX, RDX, R8, R9)
- COFF section directives (`.section .rdata`)
- Shadow space allocation (32 bytes)

**However**, it's incomplete:
- Still uses AT&T syntax (e.g., `movq %rax, %rbx`)
- Uses Linux syscalls (`syscall` instruction)
- Missing Windows-specific directives
- Incorrect stack alignment for Windows
- No SEH (Structured Exception Handling) support

---

## Required Changes

### 1. Syntax Conversion: AT&T → Intel

**Effort:** 1-1.5 weeks
**Complexity:** Medium
**Lines affected:** ~922 assembly generation statements

#### Key Differences

| AT&T Syntax | Intel Syntax |
|-------------|--------------|
| `movq %rax, %rbx` | `mov rbx, rax` |
| `movl $10, %eax` | `mov eax, 10` |
| `addl %ebx, %eax` | `add eax, ebx` |
| `pushq %rax` | `push rax` |
| `call malloc` | `call malloc` |
| `leaq .LC0(%rip), %rdi` | `lea rdi, [rel .LC0]` |
| `-8(%rbp)` | `[rbp-8]` |
| `(%rax)` | `[rax]` |

#### Implementation Strategy

**Option A: Abstraction Layer (Recommended)**
```c
// Create instruction builder functions
void emit_mov_reg_reg(Parser *parser, const char *dest, const char *src);
void emit_mov_reg_imm(Parser *parser, const char *dest, int value);
void emit_add_reg_reg(Parser *parser, const char *dest, const char *src);
// etc.
```

**Benefits:**
- Cleaner code
- Easier to maintain
- Can support both syntaxes
- Better testing

**Option B: String Replacement**
- Replace all `code_printf` calls
- Convert operand order
- Update register names
- Error-prone and tedious

**Recommended:** Use Option A with a new `instruction_builder.c` module.

#### Changes Required
1. Create instruction abstraction layer (~500 lines)
2. Update all 922 `code_printf` calls
3. Remove `%` prefix from registers
4. Reverse operand order (dest, src)
5. Convert memory addressing syntax
6. Update string literals in assembly

### 2. COFF Format Support

**Effort:** 1 week
**Complexity:** Medium
**Lines affected:** ~200 lines

#### Current vs. Required

**Current (ELF/AT&T):**
```asm
    .text
    .section .rodata
.LC_int_format:
    .ascii "%d\0"
.globl main
main:
    pushq %rbp
```

**Required (COFF/Intel):**
```asm
    .intel_syntax noprefix
    .text
    .def main
        .scl 2
        .type 32
    .endef
    .globl main
    .align 16
main:
    push rbp
```

#### Implementation Tasks
1. **File Header Generation** (main.c)
   - Add `.intel_syntax noprefix` directive
   - Update section directives (`.section` → `.data`, `.text`, `.rdata`)
   - Add COFF function definitions (`.def`, `.scl`, `.type`, `.endef`)

2. **Symbol Export** 
   - Use `.globl` or `.global` consistently
   - Add `.def` blocks for all functions
   - Specify storage class (`.scl 2` for external)

3. **Data Sections**
   - `.rodata` → `.section .rdata,"dr"`
   - `.bss` → `.section .bss,"bw"`
   - Add proper alignment directives

4. **String Literals**
   - Keep null termination
   - Update escape sequences if needed
   - Ensure proper alignment

### 3. Windows x64 ABI Implementation

**Effort:** 1-1.5 weeks
**Complexity:** High
**Lines affected:** ~300 lines

#### Key ABI Differences

| Aspect | System V (Linux) | Windows x64 |
|--------|------------------|-------------|
| **Integer args** | RDI, RSI, RDX, RCX, R8, R9 | RCX, RDX, R8, R9 |
| **Float args** | XMM0-XMM7 | XMM0-XMM3 |
| **Return value** | RAX (int), XMM0 (float) | RAX (int), XMM0 (float) |
| **Caller-saved** | RAX, RCX, RDX, RSI, RDI, R8-R11 | RAX, RCX, RDX, R8-R11 |
| **Callee-saved** | RBX, RBP, R12-R15 | RBX, RBP, RDI, RSI, R12-R15 |
| **Shadow space** | None | 32 bytes (4 × 8) |
| **Stack alignment** | 16 bytes before call | 16 bytes before call |
| **Red zone** | 128 bytes | None |

#### Critical Changes

**1. Function Prologue**
```c
// Current (Linux)
code_printf(parser, "    pushq %%rbp\n");
code_printf(parser, "    movq %%rsp, %%rbp\n");
code_printf(parser, "    subq $8192, %%rsp\n");

// Required (Windows)
emit_push(parser, "rbp");
emit_mov_reg_reg(parser, "rbp", "rsp");
emit_sub_reg_imm(parser, "rsp", 8192);
// Note: Must preserve RDI, RSI on Windows
emit_push(parser, "rdi");
emit_push(parser, "rsi");
```

**2. Function Calls**
```c
// Current (Linux) - first arg in RDI
code_printf(parser, "    movq %s, %%rdi\n", arg);
code_printf(parser, "    call printf\n");

// Required (Windows) - first arg in RCX, need shadow space
emit_sub_reg_imm(parser, "rsp", 32);  // Shadow space
emit_mov_reg_reg(parser, "rcx", arg);
emit_call(parser, "printf");
emit_add_reg_imm(parser, "rsp", 32);  // Clean up shadow space
```

**3. Variadic Functions (printf, scanf)**
- Windows requires shadow space even for variadic functions
- First 4 args in registers, rest on stack
- Must ensure 16-byte alignment

**4. Replace Linux Syscalls**
- `syscall` instruction doesn't exist on Windows
- Must use Windows API or CRT functions:
  - `sys_write` → `WriteFile` or `printf`
  - `sys_read` → `ReadFile` or `scanf`
  - Direct syscalls not portable

#### Implementation Tasks
1. Update `parser_types.c` calling convention (already partially done)
2. Modify all function call sites (~63 locations)
3. Add shadow space allocation/deallocation
4. Update function epilogues to restore RDI, RSI
5. Fix stack alignment calculation
6. Replace syscall instructions with CRT calls
7. Update built-in function handling

### 4. Testing Strategy

**Effort:** 0.5-1 week
**Complexity:** Medium

#### Test Plan
1. **Syntax Tests**
   - Verify all generated assembly uses Intel syntax
   - Check register names (no `%` prefix)
   - Validate operand order

2. **Assembler Compatibility**
   - Test with NASM on Windows
   - Test with MASM on Windows
   - Test with GAS (GNU assembler) with Intel syntax
   - Ensure linker compatibility (link.exe)

3. **Functional Tests**
   - Port all 22 existing tests to Windows
   - Test calling conventions
   - Test stack alignment
   - Test with Windows CRT

4. **Cross-Platform**
   - Maintain Linux support (conditional compilation)
   - Test both platforms
   - Verify platform detection works

#### Testing Infrastructure
```c
// Add test configuration
typedef enum {
    TARGET_LINUX_ELF_ATT,
    TARGET_LINUX_ELF_INTEL,
    TARGET_WINDOWS_COFF_INTEL
} TargetPlatform;

// Add runtime flag
TargetPlatform target = detect_target();
```

---

## Implementation Roadmap

### Phase 1: Abstraction Layer (Week 1)
**Goal:** Create instruction builder abstraction

**Tasks:**
1. Create `src/instruction_builder.c` and `.h`
2. Implement core instruction builders:
   - mov, add, sub, mul, div
   - push, pop, call, ret
   - jmp, je, jne, jg, jl, etc.
   - lea, cmp, test
3. Add conditional syntax generation (AT&T vs Intel)
4. Write unit tests for instruction builders

**Deliverable:** Working instruction abstraction that can output both syntaxes

### Phase 2: Syntax Migration (Week 2)
**Goal:** Convert all assembly generation to Intel syntax

**Tasks:**
1. Update `parser_codegen.c` (syscall and utility functions)
2. Update `parser_statements.c` (statements and control flow)
3. Update `parser_expressions.c` (expressions and operators)
4. Update `parser_declarations.c` (function prologue/epilogue)
5. Update `main.c` (file headers)
6. Remove all AT&T syntax code

**Deliverable:** Compiler outputs Intel syntax assembly

### Phase 3: COFF Format (Week 3)
**Goal:** Proper COFF output format

**Tasks:**
1. Update file header generation in `main.c`
2. Add `.def`/`.endef` blocks for functions
3. Update section directives
4. Update symbol definitions
5. Test with Windows assembler (NASM/MASM)
6. Verify object file format

**Deliverable:** Assembly files assemble with Windows tools

### Phase 4: Windows ABI (Week 3-4)
**Goal:** Correct Windows calling conventions

**Tasks:**
1. Update register allocation in `parser_types.c`
2. Add shadow space management
3. Update all function call sites
4. Update function prologue/epilogue
5. Replace syscalls with CRT functions
6. Test calling convention compliance

**Deliverable:** Windows-compliant calling conventions

### Phase 5: Testing & Validation (Week 4)
**Goal:** Comprehensive testing

**Tasks:**
1. Port all 22 tests to Windows
2. Add Windows-specific tests
3. Test with different assemblers
4. Fix bugs and edge cases
5. Performance testing
6. Documentation updates

**Deliverable:** Fully working Windows backend

---

## Risk Assessment

### High Risks
1. **Stack Alignment Issues**
   - Misaligned stacks cause crashes
   - Windows is strict about 16-byte alignment
   - **Mitigation:** Careful calculation, extensive testing

2. **Calling Convention Bugs**
   - Wrong registers cause data corruption
   - Hard to debug
   - **Mitigation:** Unit tests for each function type

3. **Register Preservation**
   - Windows requires preserving RDI, RSI
   - Forgetting causes subtle bugs
   - **Mitigation:** Add to function prologue template

### Medium Risks
1. **Assembler Compatibility**
   - Different assemblers have quirks
   - **Mitigation:** Test with multiple assemblers

2. **Testing Coverage**
   - Need Windows testing environment
   - **Mitigation:** Use CI/CD with Windows runner

3. **Backward Compatibility**
   - Breaking Linux support
   - **Mitigation:** Maintain both paths with preprocessor

### Low Risks
1. **String Formatting**
   - Escape sequences differ slightly
   - **Mitigation:** Straightforward mapping

---

## Resource Requirements

### Development Environment
- **Windows machine** with:
  - Visual Studio or MinGW-w64
  - NASM or MASM assembler
  - Windows SDK
  - Debugger (WinDbg or Visual Studio debugger)

- **Linux machine** (for comparison testing)

### Skills Required
- **Strong C programming**
- **x64 assembly** (both AT&T and Intel syntax)
- **Understanding of ABIs** and calling conventions
- **Windows PE/COFF format** knowledge
- **Linux ELF format** knowledge (for comparison)
- **Debugging skills** (GDB, WinDbg)

### Tools
- Compiler: GCC, Clang, or MSVC
- Assemblers: NASM, MASM, GAS
- Linkers: ld (Linux), link.exe (Windows)
- Debuggers: GDB, WinDbg
- Hex editors: For inspecting object files
- CI/CD: GitHub Actions with Windows and Linux runners

---

## Alternative Approaches

### Option 1: LLVM Backend (Long-term, 3-6 months)
Instead of generating assembly directly, generate LLVM IR and let LLVM handle:
- Code generation
- Optimization
- Platform targeting
- ABI compliance

**Pros:**
- Professional-grade code generation
- Automatic optimization
- Multi-platform support
- Industry standard

**Cons:**
- Significant rewrite (entire backend)
- Learning curve for LLVM
- Larger dependency
- More complex build process

### Option 2: Dual Backend Support (Recommended)
Keep both AT&T and Intel syntax support with runtime flag:
```bash
./compiler --syntax=intel --target=windows-coff input.dmm
./compiler --syntax=att --target=linux-elf input.dmm
```

**Pros:**
- Backward compatibility
- Testing flexibility
- Educational value

**Cons:**
- More maintenance
- Larger codebase

### Option 3: Assembly Post-processor
Generate AT&T syntax, then convert to Intel via external tool:
```bash
./compiler input.dmm
./att2intel input.s > input_intel.s
```

**Pros:**
- Minimal changes to compiler
- Separation of concerns

**Cons:**
- Extra tool dependency
- Doesn't solve ABI issues
- Two-stage process

---

## Success Criteria

### Minimum Viable Product (MVP)
- [ ] All assembly in Intel syntax
- [ ] Generates valid COFF object files
- [ ] Windows x64 ABI compliant
- [ ] At least 18/22 tests pass on Windows
- [ ] Can compile and run "Hello World"

### Full Success
- [ ] All 22 tests pass on Windows
- [ ] All 22 tests still pass on Linux
- [ ] Proper error handling
- [ ] Documentation updated
- [ ] No memory leaks
- [ ] No crashes
- [ ] Performance comparable to AT&T version

### Stretch Goals
- [ ] LLVM IR backend option
- [ ] ARM64 support (Windows on ARM)
- [ ] macOS support (Mach-O format)
- [ ] Optimization passes
- [ ] Debug symbol generation (PDB files)

---

## Estimated Timeline

### Conservative Estimate (Single Developer)
- **Week 1:** Abstraction layer + partial syntax conversion (40 hours)
- **Week 2:** Complete syntax conversion (40 hours)
- **Week 3:** COFF format + start Windows ABI (40 hours)
- **Week 4:** Complete Windows ABI + testing (40 hours)

**Total: 160 hours (4 weeks)**

### Optimistic Estimate (Experienced Developer)
- **Week 1:** Abstraction + syntax conversion (35 hours)
- **Week 2:** COFF + Windows ABI (40 hours)
- **Week 3:** Testing and bug fixes (30 hours)

**Total: 105 hours (2.5-3 weeks)**

### With Multiple Developers
- **Developer 1:** Instruction abstraction + syntax
- **Developer 2:** COFF format + file generation
- **Developer 3:** Windows ABI + calling conventions
- **Developer 4:** Testing infrastructure

**Total: 2-3 weeks with 3-4 developers**

---

## Recommendations

### Immediate Next Steps
1. **Set up Windows development environment**
   - Install Visual Studio or MinGW-w64
   - Install NASM assembler
   - Test basic assembly → object → executable pipeline

2. **Create proof of concept**
   - Write simple "Hello World" in Intel syntax
   - Compile with NASM
   - Link with Windows CRT
   - Verify it works

3. **Start abstraction layer**
   - Create `instruction_builder.c`
   - Implement 10-20 most common instructions
   - Test with small programs

4. **Parallel work**
   - Update documentation
   - Create test plans
   - Set up CI/CD with Windows

### Priority Order
1. **High Priority:** Instruction abstraction (enables everything else)
2. **High Priority:** Intel syntax conversion (most visible change)
3. **Medium Priority:** Windows ABI (critical for correctness)
4. **Medium Priority:** COFF format (needed for Windows)
5. **Low Priority:** Testing infrastructure (important but can be manual initially)

### Quick Wins
- Update `main.c` to add `.intel_syntax noprefix` directive
- Create instruction builder for 5-10 most common instructions
- Convert one small function (e.g., `main` prologue/epilogue)
- Test with Intel-syntax assembler

---

## Conclusion

**The backend rewrite is feasible and worthwhile.** The estimated time of 3-4 weeks for a single experienced developer is reasonable given:
- Partial Windows support already exists
- Clear scope and requirements
- Existing test infrastructure
- Modular code architecture

**Key Success Factors:**
1. Start with abstraction layer (don't do string replacement)
2. Test incrementally (don't rewrite everything then test)
3. Maintain Linux support (conditional compilation)
4. Use Windows development environment from day 1
5. Refer to MSDN documentation for Windows ABI

**This document should give another developer everything they need to:**
- Understand the scope
- Plan the implementation
- Estimate effort
- Identify risks
- Execute the rewrite

Good luck! 🚀
