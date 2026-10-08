# stdlib/internal/system

This is the default system package for DMM applications. The compiler adds it to the application's
package graph, then compiles its selected implementations together with the program. There is no required
system object, archive, bootstrap compilation step or separately installed runtime bundle.

The provider imports the executor and thread/event packages. `stdlib/net/raw` imports its network implementation
and calls it through ordinary DMM functions. These packages share the same executor/threading package instances.
Native FFI imports bind glibc/pthreads on Linux and Kernel32/UCRT/Winsock on Windows.

The compiler retains the private native ABI entry points needed by generated async frames and startup.
Their bodies are DMM functions in the package graph. Atomic machine instructions, frame lowering, ownership
rules and the existing base memory/I/O primitives remain compiler responsibilities. Unused source stdlib
functions do not change the standalone profile, including unused async features or network imports.

An installed compiler loads source from `share/dmm/stdlib` next to its `bin` directory. Development builds
fall back to the configured checkout's stdlib. Packages obey the normal target-file selection rules.
Object/assembly generation needs no target toolchain; platform executable linking still uses GCC/Clang.
Manual links need only the emitted program object and its native OS libraries, for example:

```sh
gcc -no-pie -pthread program.o -o program
gcc program.obj -lws2_32 -Wl,--subsystem,console -o program.exe
```

The old component-building mode and optional `--runtime-shim` argument remain for native ABI testing and
explicit additional native objects. CMake does not build or install runtime bundles for production.
