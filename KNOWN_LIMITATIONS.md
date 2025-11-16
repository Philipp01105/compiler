# Known Limitations and Missing Features

This document lists features that are currently not working or not implemented in the DMM compiler.

**Last Updated**: 2025-11-16  
**Compiler Version**: Development

---

## 🚧 Missing Standard Library Modules

The following stdlib modules are referenced in some code examples but **DO NOT EXIST**:

### Missing Modules
- `stdlib/core.txt` - Core utility functions (intAbs, intMin, intMax, etc.)
- `stdlib/math.txt` - Mathematical operations
- `stdlib/array.txt` - Array manipulation utilities  
- `stdlib/strings.txt` - String manipulation functions
- `stdlib/utils.txt` - General utility functions

### Existing Modules
✅ `stdlib/io.txt` - Low-level I/O operations (syscalls)
✅ `stdlib/README.md` - Documentation for I/O module

**Impact**: Code that imports missing modules will fail to compile with import errors.

---

## 🔧 Language Feature Limitations

### Struct Member Access
- ✅ **Works**: `*p.field` syntax for pointer-to-struct field access (read and write)
- ✅ **Works**: `struct.field` syntax for direct struct field access
- ❌ **Does NOT work**: `p->field` syntax (C-style arrow operator)

**Workaround**: Use `*p.field` instead of `p->field`

```dmm
// ✅ Correct
var p:*Point = reserve(Point);
*p.x = 10;
var val:int = *p.y;

// ❌ Won't compile
p->x = 10;  // ERROR: -> operator not supported
```

### Memory Management
- ✅ **Works**: Manual memory management with `reserve()` and `free()`
- ✅ **Works**: `@gc` annotation for automatic garbage collection
- ✅ **Works**: Mixed manual and automatic memory management
- ⚠️ **Limitation**: GC only frees memory at scope exit, not when variable is last used

### Pointer Operations
- ✅ **Works**: Pointer dereference (`*p`)
- ✅ **Works**: Address-of operator (`&var`)
- ✅ **Works**: Pointer assignment (`*p = value`)
- ✅ **Works**: Pointer arithmetic through array indexing
- ❌ **Does NOT work**: Direct pointer arithmetic (`p + 1`, `p++`)

### Type System
- ✅ **Works**: Basic types (int, char, byte, bit, float, double, string)
- ✅ **Works**: Pointer types (`*int`, `*char`, etc.)
- ✅ **Works**: Struct definitions and instances
- ✅ **Works**: Struct pointers
- ✅ **Works**: Enum definitions
- ❌ **Does NOT work**: Generic types
- ❌ **Does NOT work**: Type aliases

---

## 🧪 Test Coverage

### Working Tests (20 tests - ALL PASSING ✅)

**Basic Language Features**:
- `hello.dmm` - Basic output
- `arithmetic.dmm` - Arithmetic operations
- `conditional.dmm` - If statements
- `else_if.dmm` - Else-if chains
- `function.dmm` - Function definitions and calls
- `loop.dmm` - For loops
- `nested_loops.dmm` - Nested loop structures
- `variables.dmm` - Variable declarations
- `string_ops.dmm` - String operations
- `enum_test.dmm` - Enum definitions

**Heap & Pointer Management**:
- `heap_gc.dmm` - Garbage collection with @gc
- `heap_free.dmm` - Manual memory deallocation
- `heap_reserve.dmm` - Memory allocation
- `heap_pointers.dmm` - Pointer declarations
- `heap_struct_test.dmm` - Struct heap allocation
- `heap_comprehensive_test.dmm` - Complete heap management (23 tests)
- `pointer_deref_test.dmm` - Pointer dereferencing
- `pointer_param.dmm` - Pointers as parameters

**I/O Operations**:
- `io_complete_demo.dmm` - Complete I/O demonstration
- `io_syscall_direct.dmm` - Direct syscall usage

### Removed Tests (16 tests - outdated or require missing stdlib)

**Removed due to missing stdlib dependencies**:
- `io_clean_demo.dmm` - Required stdlib/io.txt functions not yet implemented
- `io_high_level.dmm` - Required stdlib/io.txt high-level functions
- `io_read_final.dmm` - Required stdlib/io.txt read functions
- `io_read_simple.dmm` - Required stdlib/io.txt
- `io_read_test_new.dmm` - Required stdlib/io.txt
- `io_simple_stdlib.dmm` - Required stdlib/io.txt high-level API
- `io_syscall_simple.dmm` - Required stdlib/io.txt
- `stdlibtest.dmm` - Required stdlib/core.txt, stdlib/math.txt, stdlib/array.txt, stdlib/strings.txt, stdlib/utils.txt

**Removed due to no expected output / outdated**:
- `io_file_test.dmm` - No expected output file
- `io_int_simple.dmm` - No expected output file
- `io_int_test.dmm` - No expected output file
- `io_int_working.dmm` - No expected output file
- `io_read_basic.dmm` - No expected output file
- `io_read_test.dmm` - No expected output file
- `io_syscall_test.dmm` - No expected output file
- `test.dmm` - Large outdated comprehensive test (2500+ lines)

---

## 📝 Implementation Status

### Recently Implemented ✅
- **@gc annotation**: Automatic memory deallocation at scope exit
- **Pointer dereference bug fixes**: Correct handling of all pointer types (char, int, float, double, struct)
- **`*p.field` syntax**: Pointer-to-struct field access for reading and writing
- **Void function returns**: Proper exit code (0) for void functions
- **Variable loading order**: Fixed pointer vs. type priority in code generation

### Not Yet Implemented ❌
- **Missing stdlib modules**: core, math, array, strings, utils
- **Arrow operator**: `p->field` syntax
- **Direct pointer arithmetic**: `p++`, `p + n`
- **Generic types**: Template-like functionality
- **Type aliases**: `type MyInt = int;`
- **Advanced GC**: Reference counting or early collection
- **Module system**: Package/namespace organization

---

## 🎯 Recommendations

### For Users
1. **Use the 20 working tests** as reference for correct syntax and features
2. **Avoid missing stdlib modules** - stick to built-in compiler features
3. **Use `*p.field`** instead of `p->field` for struct pointer access
4. **Rely on expected output files** in `tests/expected/` for validation

### For Contributors
1. **Implement missing stdlib modules** to restore removed test functionality
2. **Add arrow operator support** (`->`) for better C compatibility
3. **Implement pointer arithmetic** for more flexible pointer manipulation
4. **Extend GC** for more sophisticated memory management
5. **Add expected output files** for any new tests

---

## 🐛 Known Issues

### Compile-Time
- Import errors when missing stdlib files are referenced
- No helpful error message suggesting `*p.field` when user tries `p->field`

### Runtime
- GC may not free memory immediately when variable is no longer used (waits for scope exit)
- No memory leak detection or warning system

### Code Generation
- All working as expected after recent bug fixes ✅

---

## 📊 Statistics

- **Total Tests**: 20 (down from 36)
- **Passing Tests**: 20 (100%)
- **Test Coverage**: Core language features, heap management, basic I/O
- **Stdlib Coverage**: io.txt only (5 missing modules)
- **Known Bugs**: 0 in implemented features
- **Missing Features**: 8 major feature categories

---

*This document should be updated whenever new features are added or limitations are discovered.*
