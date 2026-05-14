# Native x86-64 backend

The compiler emits machine code directly from the same structured instructions
used by its assembly backend. Native compilation creates no assembly temporary
and invokes no assembler, external linker, or C compiler.

```sh
compiler --emit=exe --target=elf program.dmm -o program
compiler --emit=exe --target=coff program.dmm -o program.exe
compiler --emit=obj --target=elf program.dmm -o program.o
compiler --emit=obj --target=coff program.dmm -o program.obj
```

`--emit=asm` remains the default. `--target` defaults to the host platform.
Both native targets can be emitted from either host. `--syntax` controls assembly
printing and has no effect on native bytes. Native artifacts have deterministic
headers and symbol ordering, including a zero COFF timestamp. Default output
names append `.s`, `.o`/`.obj`, or `.out`/`.exe` to the source filename.

## Objects and executables

Object mode produces ELF64 ET_REL or AMD64 COFF with text, read-only data,
writable data, symbols, and relocations. Cross-section references use ELF
PC32/PLT32/64 and COFF REL32/ADDR64 relocations. Same-section branches are resolved
before serialization. These objects can be linked by ordinary platform tools;
link the target-compatible `dmm_runtime` library after the object inputs.

Executable mode uses the internal image linker to lay out the compiled module,
resolve local symbols and relocations, append startup code and native runtime
shims, and construct platform import tables. It links the compiler-generated
module and its built-in runtime; it is not a general replacement for `ld` and
does not accept arbitrary third-party object files or archives as inputs.
No separate `dmm_runtime` file is required for this mode.

Linux output is an ELF64 ET_EXEC image for x86-64 glibc systems. Its interpreter
is `/lib64/ld-linux-x86-64.so.2`; it imports `libc.so.6` through dynamic symbols,
SysV hash and RELA tables. Startup calls `__libc_start_main` with the process
arguments and loader finalizer. Code, read-only data and writable data have
separate load segments, and the stack is non-executable. Images use a fixed
base of `0x400000`. Native Linux-host builds mark ELF executables executable.

Windows output is a PE32+ console image with text, read-only data, writable
data, import and base-relocation sections. The image imports `msvcrt.dll` through
its own import descriptor, lookup and address tables. Startup calls the source
`main`, then CRT `exit` to flush output and propagate the exit code. Absolute
enum payload pointers receive DIR64 base relocations; the image enables ASLR
and NX. Native runtime wrappers translate file flags, preserve input/EOF defaults
and guarantee bounded integer-format termination.

The native runtime machine-code definitions are in `src/runtime/native_runtime.c`,
outside the IR emitter. Its system imports use private names so source functions
such as `write` cannot intercept runtime calls. Typed runtime operations remain
ordinary verified IR calls. Allocation, conversions, aggregates, SSE values and
both calling conventions reuse existing IR-to-instruction lowering.

## Validation and limits

Invalid instructions, unresolved executable symbols, relocation bounds and
out-of-range relative addresses fail with code-generation diagnostics. Loaded
sources and aliased artifact paths have the same protection in every emission
mode. `--source-map` emits `dmm-native-map-v1` with text-section byte offsets.

CTest covers native object interoperability and C ABI callers, executable
execution across the language corpus, input/EOF and truncation behavior,
syntax-independent bytes, both cross-emission targets, binary table structure,
relocation application, malformed instructions and source/artifact protection.
The existing Linux and Windows CI jobs run these tests on their respective hosts.
Local validation was performed on Windows; ELF execution must be confirmed by
the Linux job. Sanitizers instrument the compiler and C runtime library, not
the machine code in internally linked images. Native images do not currently
contain DWARF/PDB debug information or Windows exception-unwind tables.

Format references: [Microsoft PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[ELF section specification](https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.sheader.html),
[ELF relocations](https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.reloc.html),
and [Intel instruction manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
