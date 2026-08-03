# Native x86-64 backend

The compiler emits machine code directly from verified IR and structured x86-64 instructions. Native compilation invokes
no assembler, C compiler or external linker in its default standalone profile. Standalone output embeds a compiler-owned runtime: generated programs
require no libc, Windows CRT or foreign language runtime. The compiler itself remains implemented in C.

## Private platform-runtime handoff

`--link=auto|internal|external` selects the link strategy (default `auto`). Runtime requirements are separate from
link strategy: required IR operations determine a `standalone` or `platform` profile, and the driver resolves a
supported linker for that profile. Used network IR requires `NETWORK`, which implies `PLATFORM_RUNTIME`; neither an
unused import nor the `async` manifest feature requires a platform runtime. The networking ABI, combined shim and
typed Core API are specified in [NETWORK_RUNTIME.md](NETWORK_RUNTIME.md).

For this development step, explicit `--link=external` requests `PLATFORM_RUNTIME`, not `NETWORK`. `auto` selects the
internal linker for standalone output and the external driver for platform output. `internal` cannot satisfy a
platform requirement. With `--emit=obj` or `--emit=asm`, `--link` selects the runtime/ABI profile intended for later
linking; **no link process starts**, and linker-driver/shim overrides are not needed or consulted.

```sh
compiler --link=external program.dmm -o program
compiler --link=external --linker-driver /usr/bin/clang program.dmm -o program
compiler --link=external --emit=obj program.dmm -o program.o
compiler --link=external --emit=asm --syntax=att program.dmm -o program.s
```

Platform executables use the platform's C startup and documented platform libraries. Standalone startup, native
runtime and internal linking are unchanged. Platform output still embeds the generated DMM memory/I/O runtime and
scheduler, but leaves private thread/event operations to a separately compiled shim. It defines
`int __dmm_runtime_main(void)` instead of `__dmm_entry`; DMM `main` is privately named `__dmm_program_main`.
The shim's C `main` only returns `__dmm_runtime_main()`.

The generated bridge owns the lifecycle: loader-initialized runtime storage is available before package initializers;
then package initialization, DMM main, preservation of its exit code, default-executor drain, package cleanup and
return to C startup. Draining releases the default executor's runtime state. No additional eager runtime allocation
or global teardown phase is introduced. A void main returns zero. Immediate `exit`, traps and fatal failures bypass
normal DMM cleanup; platform `exit` terminates the entire process, including when called from a worker.
The shim has no package-cleanup or scheduler-lifecycle knowledge. Its private ABI is documented in
`src/runtime/platform_shim.h` and `src/runtime/EXECUTOR.md`.

The external executable path defaults to `gcc` from PATH. `--linker-driver PATH` selects a GCC-compatible driver;
`--runtime-shim PATH` selects a matching private shim object. CMake builds the shim and installs it next to the
compiler as `dmm-runtime/elf/platform-shim.o` or `dmm-runtime/coff/platform-shim.o`. Discovery uses the running
compiler's directory, including when the compiler was found through PATH. Linux GCC/Clang and Windows MinGW-w64
UCRT64 GCC/Clang are supported; MSVC and MSVCRT shims are not supported. Cross-linking requires both overrides and
a matching target toolchain. The override is a private ABI implementation, not a public arbitrary-object or FFI API.
Network requirements instead select the complete `network-shim.o` in the same directory, add Windows `ws2_32`,
and insert DRAINING before package cleanup and network shutdown afterward. Pure platform programs retain the smaller
shim and no networking dependency.

Manual linking of platform output uses regular C startup, **without** standalone entry flags or `-nostdlib`:

```sh
# Linux; substitute program.s for program.o to link assembly output.
gcc -no-pie -pthread program.o /path/to/dmm-runtime/elf/platform-shim.o -o program
# Windows UCRT64; substitute program.s for program.obj for assembly output.
gcc program.obj C:/path/to/dmm-runtime/coff/platform-shim.o -lkernel32 -Wl,--subsystem,console -o program.exe
```

Linux platform output is non-PIE, consistent with the native object's absolute data relocations. The driver invokes
the external tool with individual arguments and no shell, captures stdout/stderr and status in text/JSON diagnostics,
and publishes a temporary linked image only after success. A failed link preserves an existing requested output.
Only platform programs gain libc/UCRT and the required OS dependencies; network libraries are deferred until network
operations exist. Reproducible external linking is scoped to a fixed toolchain, shim and linker configuration.

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
Startup calls package initialization, then `main`, then normal package cleanup and ExitProcess. Absolute data pointers receive DIR64 base relocations; internally linked images
enable ASLR and NX.

Runtime generators live in `src/runtime/native_runtime.c` and `standalone.inc`, outside the IR emitter. They implement
string length/comparison/copy/append, duplication, allocation/release, decimal integer parsing and formatting, fixed
six-decimal binary64 formatting, bounded output, scalar input and file I/O. Binary64 formatting rounds ties to even and
handles subnormals, infinity, NaN and signed zero without libc. Private runtime symbols prevent source functions such as
write from intercepting platform calls. Unknown executable imports fail. Runtime operations remain typed, verified IR
calls.

Concrete aggregate ownership metadata reaches verified IR. The native backend lowers explicit `drop`, `move`, and `reinit`
effects, keeps initialization flags for `NEEDS_DROP` locals, and calls compiler-generated drop glue. Drop glue executes
the user destructor before recursively destroying owned fields and fixed-array elements in reverse order. Sum-enum
drop glue dispatches on the active tag and destroys its owned payloads in reverse order. By-value
owning parameters are cleaned up by the callee. Runtime package initializers are lowered into
`__dmm_package_init` in deterministic dependency-first order. `NEEDS_DROP` package globals have private initialization
flags and are dropped by `__dmm_package_cleanup` in reverse initialization order; owned global slice backing is released
there as well. Native startup calls initialization before `main`, invokes cleanup after a normal return, and exits with
the preserved return value; the immediate `exit` intrinsic bypasses cleanup.

`array-literal` materializes fixed arrays inline and fills slice backing by cyclically repeating its typed pattern.
Local slice backing is released by explicit `free-slice-backing` effects at the owning scope exit; returned backing is
transferred to the caller's receiving lvalue, and temporary call arguments are released only after the call. Static
package slice literals point at static data. Runtime-created backing transferred into package storage is tracked by a
private owner slot and released on reassignment or normal package cleanup.

## Validation and limits

CTest executes the full language corpus through internal executables and assembly/object links with `-nostdlib`.
Dependency checks forbid dynamic ELF loading and foreign Windows DLLs. Unit tests execute emitted runtime instructions,
compare 2,000 finite binary64 formats with a test-only reference, and cover allocation overflow, integer limits, file
operations and negative errors. Other checks cover C ABI interoperability, EOF/input/truncation, deterministic bytes,
cross-format headers/relocations and artifact/source protection.

Local Linux GCC and Windows UCRT64 GCC validation pass 41/41 suites; Windows UCRT64 Clang also passes the networking
object/assembly matrix. Linux ASan/UBSan checks the network shim and test harness with leak detection. Sanitizers
instrument C code, not emitted instructions. Hosted CI results require separate verification.

The initial allocator maps each allocation separately. Windows maintains 253 file slots plus standard descriptors; input
strings have a 255-byte limit. The internal formatter implements the fixed formats needed by DMM operations, not a
general C printf API. Images currently lack DWARF/PDB and Windows unwind tables. Native source maps contain text-section
offsets for source instructions.

Format references: [PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[ELF program headers](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html)
and [Intel instruction manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
