# DMM Compiler Error Codes

This document lists all error and warning codes used by the DMM compiler. Error codes help you quickly identify and resolve issues in your code.

## Error Code Format

Error codes follow the format `X###` where:
- `X` is a character indicating the error category
- `###` is a 3-digit number starting at 100

## Categories

- **L** - Lexer errors (tokenization phase)
- **P** - Parser errors (syntax analysis phase)
- **T** - Type errors (type checking)
- **S** - Semantic errors (semantic analysis)
- **G** - Code generation errors
- **C** - General compiler errors
- **W** - Warnings (non-fatal issues)

---

## Lexer Errors (L100-L199)

### L100 - Unclosed String Literal
**Description:** A string literal was started with `"` but not closed before the end of line or file.

**Example:**
```
var msg:string = "Hello World;  // Missing closing quote
```

**Solution:** Add the closing `"` to complete the string literal.

---

### L101 - Unclosed Character Literal
**Description:** A character literal was started with `'` but not closed properly.

**Example:**
```
var ch:char = 'A;  // Missing closing quote
```

**Solution:** Add the closing `'` to complete the character literal.

---

### L102 - Invalid Escape Sequence
**Description:** An unrecognized escape sequence was used in a string or character literal.

**Example:**
```
var ch:char = '\q';  // \q is not a valid escape sequence
```

**Solution:** Use a valid escape sequence: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`, `\'`

---

### L103 - Unknown Character
**Description:** The lexer encountered a character that is not part of the language syntax.

**Example:**
```
var x:int = 5 $ 3;  // $ is not a valid operator
```

**Solution:** Check for typos or remove invalid characters.

---

### L104 - File Not Found
**Description:** The specified source file could not be found or opened.

**Solution:** Verify the file path is correct and the file exists.

---

### L105 - File Read Error
**Description:** An error occurred while reading the source file.

**Solution:** Check file permissions and ensure the file is not corrupted.

---

## Parser Errors (P100-P199)

### P100 - Unexpected Token
**Description:** The parser encountered a token that doesn't fit the expected syntax.

**Example:**
```
func main() -> int {
    var x:int 5;  // Missing '=' before value
}
```

**Solution:** Check the syntax and ensure all required tokens are present.

---

### P101 - Expected Token
**Description:** A specific token was expected but not found.

**Example:**
```
func main() -> int
    return 0;  // Missing '{' after function signature
}
```

**Solution:** Add the missing token as indicated in the error message.

---

### P102 - Invalid Syntax
**Description:** The code contains invalid syntax that doesn't match language grammar rules.

**Solution:** Review the language documentation and fix the syntax.

---

### P103 - Missing Semicolon
**Description:** A statement is missing a required semicolon.

**Example:**
```
func main() -> int {
    var x:int = 5  // Missing semicolon
    return x;
}
```

**Solution:** Add a semicolon at the end of the statement.

---

### P104 - Missing Brace
**Description:** A required `{` or `}` is missing.

**Example:**
```
func main() -> int {
    if (x > 5)
        return 1;
    // Missing closing brace
```

**Solution:** Add the missing brace to close the block.

---

### P105 - Missing Parenthesis
**Description:** A required `(` or `)` is missing.

**Example:**
```
func main() -> int {
    if x > 5) {  // Missing opening parenthesis
        return 1;
    }
}
```

**Solution:** Add the missing parenthesis.

---

### P106 - Invalid Declaration
**Description:** A variable, function, or struct declaration is invalid.

**Example:**
```
func main() -> int {
    var :int = 5;  // Missing variable name
}
```

**Solution:** Ensure declarations follow the correct format: `var name:type`

---

### P107 - Duplicate Definition
**Description:** A function, struct, or variable has been defined more than once.

**Example:**
```
func helper() -> int { return 1; }
func helper() -> int { return 2; }  // Duplicate function
```

**Solution:** Rename one of the definitions or remove the duplicate.

---

### P108 - Too Many Errors
**Description:** The compiler stopped due to too many errors (default: 10).

**Solution:** Fix the reported errors and recompile.

---

## Type Errors (T100-T199)

### T100 - Type Mismatch
**Description:** An operation or assignment involves incompatible types.

**Example:**
```
func main() -> int {
    var x:int = "hello";  // Cannot assign string to int
    return x;
}
```

**Solution:** Ensure types match or use appropriate type conversions.

---

### T101 - Unknown Type
**Description:** A type name is not recognized.

**Example:**
```
func main() -> int {
    var x:foo;  // 'foo' is not a known type
    return 0;
}
```

**Solution:** Use a valid type: `int`, `char`, `byte`, `bit`, `float`, `double`, `string`, `void`

---

### T102 - Invalid Operation
**Description:** An operation is not valid for the given type(s).

**Example:**
```
func main() -> int {
    var x:string = "hello" + 5;  // Cannot add int to string
    return 0;
}
```

**Solution:** Use operations appropriate for the types involved.

---

### T103 - Incompatible Types
**Description:** Function argument or return type doesn't match the declaration.

**Example:**
```
func helper(x:int) -> int { return x; }

func main() -> int {
    return helper("hello");  // Passing string to int parameter
}
```

**Solution:** Pass arguments of the correct type.

---

## Semantic Errors (S100-S199)

### S100 - Undefined Variable
**Description:** A variable is used before being declared.

**Example:**
```
func main() -> int {
    x = 5;  // 'x' not declared
    return x;
}
```

**Solution:** Declare the variable with `var` before using it.

---

### S101 - Undefined Function
**Description:** A function is called but not defined anywhere.

**Example:**
```
func main() -> int {
    return helper();  // 'helper' not defined
}
```

**Solution:** Define the function or check for typos in the function name.

---

### S102 - Undefined Struct
**Description:** A struct type is referenced but not defined.

**Example:**
```
func main() -> int {
    var p:Person;  // 'Person' struct not defined
    return 0;
}
```

**Solution:** Define the struct before using it.

---

### S103 - Wrong Argument Count
**Description:** A function call has the wrong number of arguments.

**Example:**
```
func helper(x:int, y:int) -> int { return x + y; }

func main() -> int {
    return helper(5);  // Expected 2 arguments, got 1
}
```

**Solution:** Pass the correct number of arguments.

---

### S104 - Not an Array
**Description:** Array indexing `[]` is used on a non-array variable.

**Example:**
```
func main() -> int {
    var x:int = 5;
    return x[0];  // 'x' is not an array
}
```

**Solution:** Only use `[]` with arrays or strings.

---

### S105 - Not a Struct
**Description:** Member access `.` is used on a non-struct variable.

**Example:**
```
func main() -> int {
    var x:int = 5;
    return x.field;  // 'x' is not a struct
}
```

**Solution:** Only use `.` with struct instances.

---

### S106 - Field Not Found
**Description:** A struct field name doesn't exist in the struct definition.

**Example:**
```
struct Point { var x:int; var y:int; }

func main() -> int {
    var p:Point;
    return p.z;  // 'Point' has no field 'z'
}
```

**Solution:** Use a valid field name or add the field to the struct.

---

### S107 - Method Not Found
**Description:** A method name doesn't exist in the struct.

**Example:**
```
struct Point { var x:int; }

func main() -> int {
    var p:Point;
    return p.distance();  // 'Point' has no method 'distance'
}
```

**Solution:** Define the method in the struct or check for typos.

---

### S108 - Not Static
**Description:** Attempting to call a non-static method without an instance.

**Example:**
```
struct Helper {
    func process() -> int { return 1; }
}

func main() -> int {
    return Helper.process();  // 'process' is not static
}
```

**Solution:** Mark the method as `static` or call it on an instance.

---

### S109 - Break Outside Loop
**Description:** A `break` statement appears outside of a loop.

**Example:**
```
func main() -> int {
    break;  // Not inside a loop
    return 0;
}
```

**Solution:** Only use `break` inside `for` or `while` loops.

---

### S110 - Continue Outside Loop
**Description:** A `continue` statement appears outside of a loop.

**Example:**
```
func main() -> int {
    continue;  // Not inside a loop
    return 0;
}
```

**Solution:** Only use `continue` inside `for` or `while` loops.

---

## Code Generation Errors (G100-G199)

### G100 - Too Many String Literals
**Description:** The program exceeds the maximum number of string literals (5000).

**Solution:** Reduce the number of unique string literals in your program.

---

### G101 - Too Many Variables
**Description:** The program exceeds the maximum number of variables (400).

**Solution:** Reduce the number of variables or split into multiple functions.

---

### G102 - Too Many Functions
**Description:** The program exceeds the maximum number of functions (200).

**Solution:** Reduce the number of functions or split into multiple files.

---

### G103 - Output File Failed
**Description:** Could not create or write to the output assembly file.

**Solution:** Check file permissions and disk space.

---

## Compiler Errors (C100-C199)

### C100 - No Main Function
**Description:** The program does not have a `main()` function as entry point.

**Example:**
```
func helper() -> int { return 1; }
// Missing main function
```

**Solution:** Add a `main()` function with signature `func main() -> int`

---

### C101 - No Source File
**Description:** No source file was specified on the command line.

**Solution:** Provide a source file: `compiler program.dmm`

---

### C102 - Invalid Option
**Description:** An unknown command-line option was specified.

**Solution:** Use `--help` to see valid options.

---

## Warnings (W100-W199)

### W100 - Unused Variable
**Description:** A variable is declared but never used.

**Example:**
```
func main() -> int {
    var x:int = 5;  // 'x' is never used
    return 0;
}
```

**Solution:** Remove the unused variable or use it in your code.

---

### W101 - Deprecated
**Description:** Using a deprecated feature or syntax.

**Solution:** Update to the recommended replacement shown in the warning.

---

## Getting Help

If you encounter an error you don't understand:

1. Check this document for the error code
2. Read the error message carefully - it often contains helpful information
3. Use `--formatError` flag for JSON output that's easier to parse programmatically
4. Review the compiler documentation for syntax and language features

## Examples

### Viewing errors with color output (default):
```bash
$ compiler program.dmm
```

### JSON format for tooling integration:
```bash
$ compiler --formatError program.dmm
```

### With debug information:
```bash
$ compiler --debug program.dmm
```
