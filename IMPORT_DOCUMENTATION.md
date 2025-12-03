# Import Functionality Documentation

## Overview

The compiler supports importing external files using the `#import` directive. This allows you to organize code into reusable modules and create libraries that can be shared across multiple programs.

## Syntax

```javascript
#import <path/to/file.txt>
```

or

```javascript
#import "path/to/file.txt"
```

## Features

### 1. Multiple Import Paths

The compiler searches for imported files in multiple locations:

1. **Current working directory** (project root)
2. **Relative to source file's directory**
3. **Absolute paths** (if specified)

### 2. Duplicate Prevention

The compiler automatically detects and prevents duplicate imports:

```javascript
#import <stdlib/core.txt>
#import <stdlib/core.txt>  // Automatically skipped
```

This ensures:
- Functions aren't redefined
- Compilation is faster
- No naming conflicts

### 3. Nested Imports

Imported files can import other files:

**mylib.txt:**
```javascript
#import <stdlib/core.txt>

func myFunction() -> int {
    return intAbs(-42);
}
```

**main.txt:**
```javascript
#import <mylib.txt>  // Also imports stdlib/core.txt

func main() -> void {
    print("Result: " + myFunction());
}
```

### 4. Path Syntax

Imports support various path formats:

```javascript
// Relative paths
#import <stdlib/core.txt>
#import <../shared/utils.txt>

// Subdirectories
#import <lib/math/advanced.txt>

// Different extensions
#import <library.lib>
#import <module.txt>
```

The import path is built from tokens, so paths with `/`, `.`, and `-` are supported.

## Usage Examples

### Example 1: Basic Import

**math_helpers.txt:**
```javascript
func square(n:int) -> int {
    return n * n;
}

func cube(n:int) -> int {
    return n * n * n;
}
```

**main.txt:**
```javascript
#import <math_helpers.txt>

func main() -> void {
    print("Square of 5: " + square(5));
    print("Cube of 3: " + cube(3));
}
```

### Example 2: Multiple Imports

```javascript
#import <stdlib/core.txt>
#import <stdlib/math.txt>
#import <custom/validators.txt>
#import <custom/formatters.txt>

func main() -> void {
    var num:int = -42;
    var abs_val:int = intAbs(num);
    var pow_val:int = mathPow(2, 10);
    
    print("Absolute: " + abs_val);
    print("Power: " + pow_val);
}
```

### Example 3: Organizing Project Structure

```
project/
├── main.txt
├── lib/
│   ├── database.txt
│   ├── ui.txt
│   └── validation.txt
└── stdlib/
    ├── core.txt
    ├── math.txt
    └── strings.txt
```

**main.txt:**
```javascript
#import <stdlib/core.txt>
#import <lib/database.txt>
#import <lib/ui.txt>
#import <lib/validation.txt>

func main() -> void {
    // Use functions from imported modules
}
```

### Example 4: Building a Library

**mylib/constants.txt:**
```javascript
func getMaxSize() -> int {
    return 1000;
}

func getMinValue() -> int {
    return 0;
}
```

**mylib/validators.txt:**
```javascript
#import <mylib/constants.txt>

func isValidSize(size:int) -> bit {
    var max:int = getMaxSize();
    if (size > 0 && size <= max) {
        return 1;
    }
    return 0;
}
```

**mylib/utils.txt:**
```javascript
#import <mylib/constants.txt>
#import <mylib/validators.txt>

func processData(size:int) -> void {
    if (isValidSize(size) == 1) {
        print("Processing...");
    } else {
        print("Invalid size");
    }
}
```

## Compilation Process

When the compiler encounters an import:

1. **Parse Import Statement**: Extract the file path from tokens
2. **Resolve Path**: Search for the file in multiple locations
3. **Check Duplicates**: Skip if already imported
4. **Tokenize**: Create token stream for imported file
5. **Parse**: Process functions and structs from imported file
6. **Restore Context**: Return to original file
7. **Continue**: Resume parsing from where import was found

## Debug Mode

Use `--debug` flag to see import activity:

```bash
./compiler --debug main.txt
```

Output:
```
[INFO] Importing file: stdlib/core.txt
[INFO] Import complete: stdlib/core.txt
[INFO] Importing file: stdlib/math.txt
[INFO] Import complete: stdlib/math.txt
[INFO] Skipping already imported file: stdlib/core.txt
```

## Best Practices

### 1. Import Organization

Place imports at the top of your file:

```javascript
// Standard library imports
#import <stdlib/core.txt>
#import <stdlib/math.txt>

// Third-party library imports
#import <thirdparty/networking.txt>

// Project-specific imports
#import <mylib/database.txt>
#import <mylib/ui.txt>

// Your code starts here
func main() -> void {
    // ...
}
```

### 2. Avoid Circular Dependencies

**Bad:**
```javascript
// file_a.txt
#import <file_b.txt>
func funcA() -> void { funcB(); }

// file_b.txt
#import <file_a.txt>  // Circular!
func funcB() -> void { funcA(); }
```

**Good:**
```javascript
// common.txt
func helper() -> void { }

// file_a.txt
#import <common.txt>
func funcA() -> void { helper(); }

// file_b.txt
#import <common.txt>
func funcB() -> void { helper(); }
```

### 3. Create Module Hierarchies

```
project/
├── main.txt
└── lib/
    ├── core/
    │   ├── types.txt
    │   └── utils.txt
    ├── data/
    │   ├── structures.txt
    │   └── algorithms.txt
    └── io/
        ├── file.txt
        └── network.txt
```

### 4. Document Dependencies

```javascript
// main.txt
// Dependencies:
//   - stdlib/core.txt: intAbs, intMax, intMin
//   - stdlib/math.txt: mathPow, mathGCD
//   - lib/database.txt: dbConnect, dbQuery

#import <stdlib/core.txt>
#import <stdlib/math.txt>
#import <lib/database.txt>

func main() -> void {
    // Implementation
}
```

## Limitations

1. **No macro expansion**: Imports are file-level, not text substitution
2. **No conditional imports**: Cannot use `#if` with imports
3. **Compile-time only**: All imports resolved during compilation
4. **No wildcard imports**: Must specify exact file names
5. **Windows assembly target**: Generated code is for Windows x86-64

## Error Handling

### File Not Found

```
Error (Line 7, Col 1): Failed to open import file: missing_file.txt
```

**Solution**: Check file path and ensure file exists

### Invalid Path Syntax

```
Error (Line 7, Col 15): Unexpected token in import path: @
```

**Solution**: Use only letters, numbers, `/`, `.`, and `-` in paths

### Missing Closing `>`

```
Error (Line 7, Col 30): Expected '>' after filename
```

**Solution**: Ensure import statement is properly closed

## Standard Library

The compiler comes with a comprehensive standard library in the `stdlib/` directory:

```javascript
#import <stdlib/core.txt>     // 30 functions
#import <stdlib/math.txt>     // 28 functions
#import <stdlib/array.txt>    // 25 functions
#import <stdlib/strings.txt>  // 27 functions
#import <stdlib/utils.txt>    // 35 functions
```

See [stdlib/README.md](stdlib/README.md) for complete documentation.

## Future Enhancements

Planned improvements:
- Package management system
- Version specifications
- Remote imports (URLs)
- Conditional compilation
- Module namespaces
- Private/public visibility

## Examples

Complete working examples can be found in:
- `examples/import_test.txt` - Basic import demonstration
- `examples/import_duplicate_test.txt` - Duplicate detection test
- `examples/stdlib_comprehensive_test.txt` - Full stdlib usage

## Version

Import Feature Version: 1.0.0  
Compiler Version: 5.1.0+  
Date: November 2025
