# Heap Management Documentation

This document describes the heap management features implemented in the DMM compiler, including pointer types, heap allocation, manual deallocation, and garbage collection support.

## Overview

The DMM compiler now supports explicit heap memory management with three key features:

1. **Pointer Types** - Declare variables that hold memory addresses
2. **Heap Allocation** (`reserve`) - Allocate memory on the heap at runtime
3. **Manual Deallocation** (`free`) - Manually free heap-allocated memory
4. **Garbage Collection** (`@gc`) - Mark variables for automatic garbage collection

## Pointer Types

### Syntax

Pointer types are declared using the `*` prefix before the type name:

```javascript
var ptr:*int;      // Pointer to int
var charPtr:*char; // Pointer to char
var strPtr:*string; // Pointer to string
```

### Key Points

- Pointers are 64-bit memory addresses (8 bytes on x64 architecture)
- Pointers must be explicitly declared with a type
- Uninitialized pointers contain undefined values
- Pointers can point to any data type including primitives and structs

### Example

```javascript
func main() -> void {
    var p:*int;
    var q:*char;
    var r:*string;
    
    println("Pointer types declared successfully");
}
```

## Heap Allocation with `reserve`

### Syntax

The `reserve` keyword allocates memory on the heap:

```javascript
reserve variable_name(size_in_bytes);
```

### Parameters

- `variable_name` - Must be a previously declared pointer variable
- `size_in_bytes` - Expression that evaluates to the number of bytes to allocate

### Behavior

- Calls the system `malloc()` function internally
- Returns a pointer to the allocated memory
- The allocated memory is uninitialized (contains garbage values)
- Memory remains allocated until explicitly freed or program terminates

### Example

```javascript
func main() -> void {
    var p:*int;
    reserve p(4);  // Allocate 4 bytes (1 int)
    
    println("Memory reserved successfully");
}
```

### Advanced Example - Allocating Arrays

```javascript
func main() -> void {
    var arr:*int;
    var size:int = 10;
    
    // Allocate space for 10 integers (40 bytes)
    reserve arr(size * 4);
    
    println("Array allocated on heap");
}
```

## Manual Deallocation with `free`

### Syntax

The `free` keyword deallocates heap memory:

```javascript
free variable_name;
```

### Parameters

- `variable_name` - Must be a pointer variable that was allocated with `reserve`

### Behavior

- Calls the system `free()` function internally
- Releases the memory back to the system
- After freeing, the pointer becomes invalid (dangling pointer)
- Attempting to free the same pointer twice causes undefined behavior

### Important Notes

⚠️ **Warning**: You cannot use `free` on variables marked with `@gc` - the compiler will error

### Example

```javascript
func main() -> void {
    var p:*int;
    reserve p(4);
    
    println("Memory allocated");
    
    free p;
    
    println("Memory freed");
}
```

## Garbage Collection with `@gc`

### Syntax

The `@gc` annotation marks a variable for automatic garbage collection:

```javascript
@gc
var variable_name:*type;
```

### Behavior

- Variables marked with `@gc` are tracked by the garbage collector
- Memory is automatically freed when the variable goes out of scope
- You **cannot** manually `free` a `@gc` variable
- Garbage collection prevents memory leaks for managed variables

### Example

```javascript
func main() -> void {
    @gc
    var p:*int;
    reserve p(4);
    
    println("GC-managed memory allocated");
    // No free needed - automatically garbage collected
}
```

### When to Use GC vs Manual Management

**Use `@gc` when:**
- You want automatic memory management
- Memory lifetime is tied to variable scope
- You want to prevent memory leaks

**Use manual `free` when:**
- You need precise control over memory lifetime
- You're managing large allocations that should be freed early
- You're implementing custom memory pooling or caching

## Complete Examples

### Example 1: Basic Heap Allocation

```javascript
func main() -> void {
    var num:*int;
    reserve num(4);
    
    println("Allocated 4 bytes on heap");
    
    free num;
    println("Freed memory");
}
```

### Example 2: Multiple Allocations

```javascript
func main() -> void {
    var a:*int;
    var b:*char;
    var c:*double;
    
    reserve a(4);      // 4 bytes for int
    reserve b(1);      // 1 byte for char
    reserve c(8);      // 8 bytes for double
    
    println("Multiple allocations successful");
    
    free a;
    free b;
    free c;
    
    println("All memory freed");
}
```

### Example 3: Garbage Collected Variables

```javascript
func processData() -> void {
    @gc
    var buffer:*char;
    reserve buffer(1024);  // 1KB buffer
    
    println("Processing data...");
    // Do work with buffer
    
    // No free needed - automatically cleaned up at end of function
}

func main() -> void {
    processData();
    println("Memory automatically freed after function");
}
```

### Example 4: Mixed GC and Manual Management

```javascript
func main() -> void {
    // Manually managed pointer
    var manualPtr:*int;
    reserve manualPtr(4);
    
    // GC-managed pointer
    @gc
    var gcPtr:*int;
    reserve gcPtr(4);
    
    println("Both types allocated");
    
    free manualPtr;  // Manual free required
    // gcPtr automatically freed at end of scope
}
```

## Error Handling

### Compile-Time Errors

The compiler will generate errors for:

1. **Using reserve on non-pointer variables**
   ```javascript
   var x:int;
   reserve x(4);  // ERROR: reserve can only be used with pointer variables
   ```

2. **Using free on non-pointer variables**
   ```javascript
   var x:int;
   free x;  // ERROR: free can only be used with pointer variables
   ```

3. **Using free on @gc variables**
   ```javascript
   @gc
   var p:*int;
   reserve p(4);
   free p;  // ERROR: Cannot manually free a variable marked with @gc
   ```

4. **Using reserve on undefined variables**
   ```javascript
   reserve undefined_var(4);  // ERROR: Variable 'undefined_var' not found
   ```

### Runtime Considerations

- **Null Pointers**: Freeing a NULL pointer is safe (no operation)
- **Double Free**: Freeing the same pointer twice causes undefined behavior
- **Memory Leaks**: Forgetting to free non-@gc pointers causes memory leaks
- **Dangling Pointers**: Using a pointer after freeing it causes undefined behavior

## Best Practices

1. **Initialize pointers before use**
   ```javascript
   var p:*int;
   reserve p(4);  // Initialize before using
   ```

2. **Free memory when done**
   ```javascript
   var p:*int;
   reserve p(100);
   // ... use p ...
   free p;  // Free when done
   ```

3. **Use @gc for simple cases**
   ```javascript
   @gc
   var temp:*int;
   reserve temp(4);
   // Automatically cleaned up
   ```

4. **Match reserve with free**
   ```javascript
   var p:*int;
   reserve p(4);
   // ... use p ...
   free p;  // Always pair reserve with free
   ```

5. **Set pointers to NULL after freeing** (manual)
   ```javascript
   free p;
   // In future versions, set p = NULL here
   ```

## Implementation Details

### Internal Representation

- **Pointer variables**: Stored as 8-byte addresses on the stack
- **@gc flag**: Tracked in the Variable metadata structure
- **Heap flag**: Variables marked as heap-allocated when `reserve` is used

### Code Generation

- **reserve**: Generates a call to `malloc()` with appropriate calling convention
  - Linux (System V AMD64): First argument in `%rdi`
  - Windows (x64): First argument in `%rcx`

- **free**: Generates a call to `free()` with appropriate calling convention
  - Linux: Pointer in `%rdi`
  - Windows: Pointer in `%rcx`

### Platform Compatibility

- Works on both Linux and Windows x64 platforms
- Uses platform-specific calling conventions automatically
- Generated assembly follows AT&T syntax

## Future Enhancements

The following features are planned for future versions:

1. **Automatic GC Implementation**: Currently @gc just tracks variables; full GC runtime not yet implemented
2. **Pointer Arithmetic**: Support for operations like `ptr + 1`, `ptr - 1`
3. **Dereference Operator**: Support for `*ptr` to access value at pointer
4. **Address-of Operator**: Support for `&var` to get address of variable
5. **NULL Constant**: Built-in NULL pointer constant
6. **Array Indexing on Pointers**: Support for `ptr[index]` syntax

## Version History

- **v4.1.0** (2025-11-12): Initial implementation of heap management features
  - Added pointer type declarations
  - Implemented `reserve` keyword
  - Implemented `free` keyword
  - Added `@gc` annotation support

## Related Documentation

- [Language Documentation](LANGUAGE_DOCUMENTATION.md) - Complete language reference
- [Type System](LANGUAGE_DOCUMENTATION.md#data-types) - Information about data types
- [Memory Model](LANGUAGE_DOCUMENTATION.md#memory) - Stack and heap memory model

## License

This documentation is part of the DMM Compiler project.
