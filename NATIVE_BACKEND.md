# Native x86-64 backend

The compiler emits machine code directly from verified IR and structured x86-64 instructions. Native compilation invokes
no assembler, C compiler or external linker. Every output mode embeds a compiler-owned runtime: generated programs
require no libc, Windows CRT or foreign language runtime. The compiler itself remains implemented in C.

```sh
compiler --emit=exe --target=elf program.dmm -o program
compiler --emit=exe --target=coff program.dmm -o program.exe
compiler --emit=obj --target=elf program.dmm -o program.o
compiler --emit=obj --target=coff program.dmm -o program.obj
```

Executable output (`--emit=exe`) is the default; the target defaults to the host. Use `-c` / `--emit=obj` for objects or
`-S` / `--emit=asm` for assembly. Default filenames append `.out` / `.exe`, `.o` / `.obj`, or `.s` to the source. Both
targets can be emitted from either host. Syntax selection affects assembly printing, not native bytes. Native headers
and symbol ordering are deterministic.

## Objects and assembly

Objects are ELF64 ET_REL or AMD64 COFF with text, read-only data, writable data, symbols and relocations. References use
ELF PC32/PLT32/64 or COFF REL32/ADDR64. Assembly embeds the same runtime instructions as byte directives and symbolic
relocations. No separate runtime archive or target C toolchain is needed to generate either format.

To link an object or assembly file with GNU tools, supply the own entry point:

```sh
# Linux
gcc -nostdlib -no-pie -Wl,-e,__dmm_entry program.o -o program
# Windows / MinGW
gcc -nostdlib -Wl,--entry=__dmm_entry,--subsystem,console program.obj -lkernel32 -o program.exe
```

The same commands accept generated `.s` files. C ABI interoperability tests deliberately link a C caller with its host
runtime; this is optional and does not make libc/CRT a requirement for DMM output.

## Internal executables

The internal linker combines the generated module and built-in runtime, resolves symbols/relocations, and constructs
executable headers. It does not accept arbitrary external objects or archives.

Linux output is static ELF64 ET_EXEC at base `0x400000`, with separate code, read-only and writable load segments and a
non-executable stack. There is no PT_INTERP, PT_DYNAMIC or dynamic library dependency. Startup calls DMM main and exits
with syscall 60. Allocation and file/console I/O use Linux syscalls.

Windows output is a PE32+ console image. Only `kernel32.dll` is imported:
VirtualAlloc, VirtualFree, GetStdHandle, ReadFile, WriteFile, CreateFileA, CloseHandle, GetLastError and ExitProcess.
Startup calls main and ExitProcess. Absolute data pointers receive DIR64 base relocations; internally linked images
enable ASLR and NX.

Runtime generators live in `src/runtime/native_runtime.c` and `standalone.inc`, outside the IR emitter. They implement
string length/comparison/copy/append, duplication, allocation/release, decimal integer parsing and formatting, fixed
six-decimal binary64 formatting, bounded output, scalar input and file I/O. Binary64 formatting rounds ties to even and
handles subnormals, infinity, NaN and signed zero without libc. Private runtime symbols prevent source functions such as
write from intercepting platform calls. Unknown executable imports fail. Runtime operations remain typed, verified IR
calls.

Concrete struct ownership metadata reaches verified IR. The native backend lowers explicit `drop`, `move`, and `reinit`
effects, keeps initialization flags for `NEEDS_DROP` locals, and calls compiler-generated drop glue. Drop glue executes
the user destructor before recursively destroying owned fields and fixed-array elements in reverse order. By-value
owning parameters are cleaned up by the callee. `NEEDS_DROP` package globals have private initialization flags and are
dropped by `__dmm_package_cleanup` in reverse declaration order. Native startup calls that function after a normal
return from `main` and then exits with the preserved return value; the immediate `exit` intrinsic bypasses it.

`array-literal` materializes fixed arrays inline and fills slice backing by cyclically repeating its typed pattern.
Local slice backing is released by explicit `free-slice-backing` effects at the owning scope exit; returned backing is
transferred to the caller's receiving lvalue, and temporary call arguments are released only after the call. Package
slice literals use static data and therefore require no runtime release.

## Validation and limits

CTest executes the full language corpus through internal executables and assembly/object links with `-nostdlib`.
Dependency checks forbid dynamic ELF loading and foreign Windows DLLs. Unit tests execute emitted runtime instructions,
compare 2,000 finite binary64 formats with a test-only reference, and cover allocation overflow, integer limits, file
operations and negative errors. Other checks cover C ABI interoperability, EOF/input/truncation, deterministic bytes,
cross-format headers/relocations and artifact/source protection.

Windows validation passes 26/26 suites and a strict GCC compiler build. Linux execution, sanitizer and Clang fuzz
acceptance remain for existing CI jobs. Sanitizers instrument the compiler and C test harnesses, not emitted
instructions.

The initial allocator maps each allocation separately. Windows maintains 253 file slots plus standard descriptors; input
strings have a 255-byte limit. The internal formatter implements the fixed formats needed by DMM operations, not a
general C printf API. Images currently lack DWARF/PDB and Windows unwind tables. Native source maps contain text-section
offsets for source instructions.

Format references: [PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[ELF program headers](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html)
and [Intel instruction manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
