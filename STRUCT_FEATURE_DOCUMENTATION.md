# Structs with Methods Feature Documentation

## Overview

This document describes the implementation of object-like structs with methods in the compiler. This feature allows defining structs with both fields and methods, providing OOP-like capabilities while maintaining the simplicity of a C-based syntax.

## Syntax

### Struct Definition

```c
struct StructName {
    // Fields (C-style or with var keyword)
    int field1;
    float field2;
    var field3:string;
    
    // Methods
    func methodName(param1:int, param2:float) -> returnType {
        // Method body can access fields directly
        field1 = param1;
        field2 = param2;
    }
}
```

### Field Declaration Styles

Inside structs, fields can be declared in three ways:

1. **C-style**: `int x;`
2. **With var keyword**: `var x:int;`
3. **Name-first**: `x:int;`

### Variable Declaration

```c
var instanceName:StructName;
```

### Field Access

```c
instanceName.fieldName
```

### Field Assignment

```c
instanceName.fieldName = value;
```

### Method Call

```c
instanceName.methodName(arg1, arg2);
```

### Inside Methods

Inside method bodies, fields can be accessed directly without the `this.` prefix:

```c
func move(dx:int, dy:int) -> void {
    x = x + dx;  // Direct field access
    y = y + dy;  // No need for this.x or this.y
}
```

## Example

```c
struct Point {
    int x;
    int y;
    
    func move(dx:int, dy:int) -> void {
        x = x + dx;
        y = y + dy;
    }
    
    func getX() -> int {
        return x;
    }
    
    func getY() -> int {
        return y;
    }
}

func main() -> void {
    var p:Point;
    
    // Initialize fields
    p.x = 10;
    p.y = 20;
    
    print("Initial position:");
    print("  x = " + p.x);
    print("  y = " + p.y);
    
    // Call move method
    p.move(5, 4);
    
    print("After moving by (5, 4):");
    print("  x = " + p.getX());
    print("  y = " + p.getY());
}
```

## Implementation Details

### Lexer Changes

- Added `TOKEN_KEYWORD_STRUCT` for the `struct` keyword
- Added `TOKEN_DOT` for the dot operator (`.`)

### Parser Changes

#### Type Definitions

- `StructDefinition`: Stores struct name, fields, methods, and total size
- `StructField`: Stores field name, type, offset, and size
- `Variable.struct_type`: Added to track if a variable is of a struct type
- `Function.struct_name`: Added to track if a function is a method
- `Parser.current_struct_context`: Tracks the current struct context for implicit field access

#### Parsing Functions

- `parse_struct()`: Parses struct definitions including fields and methods
- `find_struct()`: Finds a struct definition by name
- Updated `parse_variable_declaration()` to support struct types
- Updated `parse_assignment()` to support field assignment
- Updated `parse_statement()` to recognize field assignments and method calls
- Updated `parse_function_call_statement()` to support method calls
- Updated `parse_primary()` to support field access and method calls in expressions
- Updated `parse_print_statement()` to support printing fields and method results

### Code Generation

#### Method Name Mangling

Methods are compiled as regular functions with name mangling:
- Original: `Point.move()`
- Mangled: `Point_move`

#### Implicit 'this' Pointer

Methods receive an implicit first parameter called `this` which is a pointer to the struct instance:
- The address of the struct is passed in `%rcx` (Windows x64 calling convention)
- Inside the method, fields are accessed through this pointer

#### Field Access

Fields are accessed using the struct base address plus the field offset:
```assembly
movq -8(%rbp), %rbx      # Load struct base address (from 'this')
movl 0(%rbx), %eax       # Load field at offset 0
```

#### Struct Layout

Structs are allocated on the stack with proper alignment:
- Fields are laid out sequentially
- 4-byte alignment between fields
- Total struct size is aligned to 8 bytes

#### Example Generated Assembly

For the `Point` struct:
```assembly
# Method Point.move(dx:int, dy:int) -> void
.globl Point_move
Point_move:
    pushq %rbp
    movq %rsp, %rbp
    subq $8192, %rsp
    movq %rcx, -8(%rbp)     # Save 'this' pointer
    movl %edx, -16(%rbp)    # Save dx parameter
    movl %r8d, -20(%rbp)    # Save dy parameter
    
    # x = x + dx
    movq -8(%rbp), %rbx     # Load 'this'
    movl 0(%rbx), %eax      # Load x field
    # ... addition code ...
    movq -8(%rbp), %rbx     # Load 'this'
    movl %eax, 0(%rbx)      # Store back to x field
    
    # Similar for y = y + dy
    # ...
    
    leave
    ret

# Method call: p.move(5, 4)
    movl $5, %eax
    pushq %rax
    movl $4, %eax
    pushq %rax
    popq %rax
    movl %eax, %r8d         # Second argument in %r8d
    popq %rax
    movl %eax, %edx         # First argument in %edx
    leaq -8(%rbp), %rcx     # Load address of struct 'p' into %rcx (this pointer)
    subq $40, %rsp
    call Point_move
    addq $40, %rsp
```

## Limitations

1. **No inheritance**: Structs cannot extend other structs
2. **No method overloading**: Methods must have unique names within a struct
3. **No static methods**: All methods require an instance
4. **No constructors/destructors**: No special initialization or cleanup methods
5. **No access modifiers**: All fields and methods are public
6. **No nested structs**: Struct fields cannot be of another struct type (currently)

## Future Enhancements

Potential future improvements:
- Constructor and destructor support
- Static methods
- Method overloading
- Nested structs (struct fields of other struct types)
- Access modifiers (public, private)
- Simple inheritance
- Operator overloading

## Testing

The feature has been tested with:
- Basic struct definition with multiple fields
- Multiple methods per struct
- Field access and assignment
- Method calls with multiple parameters
- Methods returning values
- Methods accessing and modifying fields
- Printing field values and method results

Test file: `struct_test.txt`

## Compatibility

This feature is backward compatible with existing code. Programs that don't use structs will continue to work as before.

## Technical Notes

### Memory Layout Example

For `struct Point { int x; int y; }`:
```
Stack layout (growing downward):
-8(%rbp)  : y field (4 bytes)
-4(%rbp)  : x field (4 bytes)
0(%rbp)   : base pointer
```

### Calling Convention

Methods follow the Windows x64 calling convention:
- First 4 integer arguments: %rcx, %rdx, %r8, %r9
- First 4 float arguments: %xmm0, %xmm1, %xmm2, %xmm3
- Additional arguments on stack
- Return value in %rax or %xmm0

For methods:
- %rcx always contains the implicit 'this' pointer
- Remaining arguments follow standard convention

### Method Resolution

Method lookup:
1. Check if variable is of struct type
2. Look up struct definition
3. Generate mangled method name: `StructName_MethodName`
4. Call the mangled function with struct address as first argument
