# Input Reading Implementation Plan

**Branch**: `input-reading`  
**Author**: Philipp01105  
**Date**: 2025-12-27  
**Status**: Planning Phase

---

## 📋 Executive Summary

This document outlines the plan to implement a clean, object-oriented approach to input reading in the DMM compiler:
1. **Stream-based I/O** - Unified stream abstraction for console and files
2. **Built-in stream constants** - `sysin`, `sysout`, `syserr` as compiler constants
3. **High-level API** - No direct syscall exposure to developers
4. **Object-oriented interface** - Stream objects with methods like `readLine()`, `readInt()`, etc.

### Design Philosophy

**Clean API:** Developers should never touch syscalls directly. The compiler provides high-level abstractions.

**Stream Objects:** Everything is a stream - console input, file input, console output, file output.

**Consistent Interface:** Same methods work on all stream types (stdin or file).

---

## 🎯 Goals and Objectives

### Primary Goals

1. **Create Stream type** - New built-in type representing input/output streams
2. **Implement `read()` function** - Opens streams for stdin or files
3. **Add stream constants** - `sysin`, `sysout`, `syserr` as compiler-recognized constants
4. **Stream methods** - `readLine()`, `readInt()`, `readChar()`, `readFloat()`, `close()`
5. **Hide syscalls** - Remove direct access to `sys_read()`, `sys_open()`, etc.

### Secondary Goals

1. Add write methods for streams (`writeLine()`, `write()`)
2. Implement file writing with `write(path)` function
3. Add stream properties (`eof`, `error`, `position`)
4. Create comprehensive examples and documentation

---

## 🏗️ Architecture Design

### 1. API Hierarchy

```
┌──────────────────────────────────────────────────────┐
│                 User-Facing API                      │
├──────────────────────────────────────────────────────┤
│ Constants:                                           │
│   sysin  - Standard input stream                     │
│   sysout - Standard output stream                    │
│   syserr - Standard error stream                     │
│                                                      │
│ Functions:                                           │
│   read(sysin) -> Stream     - Open stdin stream     │
│   read("file.txt") -> Stream - Open file stream     │
│   write("file.txt") -> Stream - Open output stream  │
│                                                      │
│ Stream Methods:                                      │
│   stream.readLine() -> string                        │
│   stream.readInt() -> int                            │
│   stream.readChar() -> char                          │
│   stream.readFloat() -> float                        │
│   stream.readDouble() -> double                      │
│   stream.close() -> void                             │
│   stream.eof() -> bit                                │
└──────────────────────────────────────────────────────┘
                          ↓
┌──────────────────────────────────────────────────────┐
│              Compiler Internal Layer                 │
├──────────────────────────────────────────────────────┤
│ - Stream type implementation                         │
│ - Syscall wrappers (hidden from user)               │
│ - Buffer management                                  │
│ - Type conversion utilities                          │
└──────────────────────────────────────────────────────┘
                          ↓
┌──────────────────────────────────────────────────────┐
│              Assembly Generation                     │
├──────────────────────────────────────────────────────┤
│ Direct syscall generation (internal only)            │
│ x86-64 assembly with proper register usage           │
│ Stack management for Stream objects                  │
└──────────────────────────────────────────────────────┘
```

---

## 🔧 Implementation Details

### Phase 1: Stream Type and Constants

**New Built-in Type:**
```javascript
Stream  // Built-in opaque type (internally stores file descriptor + state)
```

**Built-in Constants:**
```javascript
sysin   // Constant representing stdin (value: special marker)
sysout  // Constant representing stdout (value: special marker)
syserr  // Constant representing stderr (value: special marker)
```

**Implementation Steps:**

1. **Type System** (`compiler_types.h`, `parser_types.c`)
   - Add `TYPE_STREAM` enum value
   - Add `TYPE_STREAM_CONSTANT` for sysin/sysout/syserr
   - Stream internally stores: file descriptor (int), buffer, position, eof flag

2. **Lexer** (`lexer.c`)
   - Recognize `sysin`, `sysout`, `syserr` as keywords/constants
   - Add to keyword table

3. **Parser** (`parser_core.c`, `parser_expressions.c`)
   - Handle stream constant references in expressions
   - Validate stream usage (only with read/write functions and methods)

**Example Code:**
```javascript
func main() -> void {
    // sysin is a built-in constant
    println("sysin is ready");  // Just checking it exists
}
```

---

### Phase 2: The `read()` Function - Stream Factory

**Function Signature (overloaded):**
```javascript
read(sysin) -> Stream           // Open stdin for reading
read(filename:string) -> Stream  // Open file for reading
```

**Behavior:**
- If parameter is `sysin` constant: returns Stream wrapping stdin (fd 0)
- If parameter is string: opens file and returns Stream wrapping file descriptor
- Returns invalid stream on error (can check with stream.eof())

**Implementation:**

1. **Parser** (`parser_expressions.c`)
   - Detect `read()` function calls
   - Check if argument is `sysin` constant or string
   - Generate appropriate code for each case

2. **Code Generation** (`parser_codegen.c`)
   ```c
   void generate_read_sysin(Parser* parser) {
       // Create Stream struct on stack
       // Set fd = 0 (stdin)
       // Initialize buffer and state
   }
   
   void generate_read_file(Parser* parser, const char* filename) {
       // Call open syscall (hidden)
       // Create Stream struct
       // Set fd to opened file descriptor
       // Initialize buffer and state
   }
   ```

3. **Stream Structure** (runtime representation)
   ```
   Stream {
       fd: int           // File descriptor (8 bytes)
       buffer: [4096]char // Internal buffer
       pos: int          // Current position in buffer
       buffered: int     // Bytes in buffer
       eof: bit          // EOF flag
   }
   // Total: ~4120 bytes on stack
   ```

**Example Code:**
```javascript
func main() -> void {
    // Read from stdin
    var input:Stream = read(sysin);
    
    // Read from file
    var file:Stream = read("data.txt");
    
    // Streams are now ready for reading
}
```

---

### Phase 3: Stream Methods

Stream objects have methods that can be called with dot notation.

#### 3.1 `stream.readLine()` - Read Full Line

**Signature:**
```javascript
func readLine() -> string
```

**Description:**
- Reads entire line from stream until newline (`\n`)
- Returns string with newline removed
- Returns empty string on EOF
- Automatically manages internal buffer

**Implementation:**
- Method call generates assembly that:
  - Accesses Stream struct (via `this` pointer)
  - Reads from fd using internal buffer
  - Accumulates characters until `\n` or EOF
  - Allocates string on heap and returns pointer

**Example:**
```javascript
func main() -> void {
    var input:Stream = read(sysin);
    
    println("Enter your name:");
    var name:string = input.readLine();
    println("Hello, " + name + "!");
}
```

#### 3.2 `stream.readInt()` - Read Integer

**Signature:**
```javascript
func readInt() -> int
```

**Description:**
- Reads characters and parses as integer
- Skips leading whitespace
- Stops at non-digit character
- Handles negative numbers

**Example:**
```javascript
func main() -> void {
    var input:Stream = read(sysin);
    
    println("Enter a number:");
    var num:int = input.readInt();
    println("Double: " + (num * 2));
}
```

#### 3.3 `stream.readChar()` - Read Single Character

**Signature:**
```javascript
func readChar() -> char
```

**Description:**
- Reads single character from stream
- Does not skip whitespace
- Returns '\0' on EOF

**Example:**
```javascript
func main() -> void {
    var input:Stream = read(sysin);
    
    println("Enter a character:");
    var ch:char = input.readChar();
    println("You entered: " + ch);
}
```

#### 3.4 `stream.readFloat()` - Read Float

**Signature:**
```javascript
func readFloat() -> float
```

**Description:**
- Reads and parses floating-point number
- Handles decimal point and negative numbers
- Skips leading whitespace

**Example:**
```javascript
func main() -> void {
    var input:Stream = read(sysin);
    
    println("Enter a decimal number:");
    var pi:float = input.readFloat();
    println("Value: " + pi);
}
```

#### 3.5 `stream.readDouble()` - Read Double

**Signature:**
```javascript
func readDouble() -> double
```

**Description:**
- Same as `readFloat()` but returns double precision

#### 3.6 `stream.close()` - Close Stream

**Signature:**
```javascript
func close() -> void
```

**Description:**
- Closes the underlying file descriptor
- For files only (closing sysin is ignored)
- Frees internal buffer

**Example:**
```javascript
func main() -> void {
    var file:Stream = read("data.txt");
    var line:string = file.readLine();
    file.close();
    
    println(line);
}
```

#### 3.7 `stream.eof()` - Check End of File

**Signature:**
```javascript
func eof() -> bit
```

**Description:**
- Returns 1 if stream reached EOF, 0 otherwise
- Useful for reading until end of file

**Example:**
```javascript
func main() -> void {
    var file:Stream = read("data.txt");
    
    for (var eof:bit = file.eof(); eof == 0; eof = file.eof()) {
        var line:string = file.readLine();
        println(line);
    }
    
    file.close();
}
```

---

### Phase 4: Complete Examples

#### Example 1: Reading from Console

```javascript
func main() -> void {
    var input:Stream = read(sysin);
    
    println("Enter your name:");
    var name:string = input.readLine();
    
    println("Enter your age:");
    var age:int = input.readInt();
    
    println("Hello, " + name + "! You are " + age + " years old.");
}
```

**Run:**
```bash
$ ./program
Enter your name:
Alice
Enter your age:
25
Hello, Alice! You are 25 years old.
```

#### Example 2: Reading from File

```javascript
func main() -> void {
    var file:Stream = read("data.txt");
    
    if (file.eof()) {
        println("Failed to open file!");
        return;
    }
    
    println("Reading file line by line:");
    for (var done:bit = file.eof(); done == 0; done = file.eof()) {
        var line:string = file.readLine();
        println(line);
    }
    
    file.close();
}
```

#### Example 3: Simple Calculator

```javascript
func main() -> void {
    var input:Stream = read(sysin);
    
    println("Calculator");
    println("Enter first number:");
    var a:int = input.readInt();
    
    println("Enter second number:");
    var b:int = input.readInt();
    
    println("Sum: " + (a + b));
    println("Product: " + (a * b));
}
```

#### Example 4: Processing CSV File

```javascript
func main() -> void {
    var file:Stream = read("data.csv");
    
    if (file.eof()) {
        println("Could not open file");
        return;
    }
    
    var total:int = 0;
    var count:int = 0;
    
    for (var done:bit = file.eof(); done == 0; done = file.eof()) {
        var line:string = file.readLine();
        // In real implementation, would split by comma
        var value:int = string_to_int(line);
        total = total + value;
        count = count + 1;
    }
    
    file.close();
    
    if (count > 0) {
        var avg:int = total / count;
        println("Average: " + avg);
    }
}
```

---

### Phase 5: Writing to Files/Streams (Optional - Future Work)

#### 5.1 `write()` Function

**Signature:**
```javascript
write(filename:string) -> Stream  // Open file for writing
write(sysout) -> Stream           // Get stdout stream
write(syserr) -> Stream           // Get stderr stream
```

**Example:**
```javascript
func main() -> void {
    var out:Stream = write("output.txt");
    out.writeLine("Hello, file!");
    out.close();
}
```

#### 5.2 Stream Write Methods

```javascript
stream.writeLine(text:string) -> void
stream.write(text:string) -> void
stream.writeInt(value:int) -> void
```

**Note:** This phase is optional and can be implemented after the reading functionality is complete and tested.

---

## 📝 Code Changes Required

### File: `src/compiler_types.h`

**Add new type:**
```c
typedef enum {
    TYPE_INT,
    TYPE_CHAR,
    TYPE_BYTE,
    TYPE_BIT,
    TYPE_STRING,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_STREAM,        // NEW: Stream type
    TYPE_STREAM_CONST,  // NEW: For sysin, sysout, syserr
    TYPE_VOID,
    // ... existing types
} DataType;

// Stream structure (internal representation)
typedef struct {
    int fd;           // File descriptor
    char buffer[4096]; // Internal buffer
    int pos;          // Current position
    int buffered;     // Bytes in buffer
    bool eof_flag;    // EOF reached
} StreamState;
```

### File: `src/lexer.c`

**Add keywords:**
```c
// In keyword table (around line 50-80)
{"sysin", TOKEN_STREAM_CONST},
{"sysout", TOKEN_STREAM_CONST},
{"syserr", TOKEN_STREAM_CONST},
{"Stream", TOKEN_TYPE},  // Stream as a type keyword
```

### File: `src/parser_types.c`

**Modify:** `is_builtin_function()`

**Add:**
```c
strcmp(name, "read") == 0 ||   // read(sysin) or read("file")
strcmp(name, "write") == 0     // write("file") - for future
```

**Add:** `is_stream_constant()`

```c
bool is_stream_constant(const char* name) {
    return strcmp(name, "sysin") == 0 ||
           strcmp(name, "sysout") == 0 ||
           strcmp(name, "syserr") == 0;
}
```

### File: `src/parser_expressions.c`

**Location:** In `parse_primary_expression()` (around line 400-800)

**Changes:**

1. **Handle stream constants:**
```c
if (is_stream_constant(token.value)) {
    // Push stream constant identifier
    // Type: TYPE_STREAM_CONST
}
```

2. **Handle `read()` function:**
```c
if (strcmp(name, "read") == 0) {
    // Parse argument (sysin or string)
    Token arg = parser->current_token;
    
    if (is_stream_constant(arg.value)) {
        generate_read_sysin(parser);  // Returns Stream
    } else if (arg.type == TOKEN_STRING) {
        generate_read_file(parser, arg.value);  // Returns Stream
    }
    
    return TYPE_STREAM;
}
```

3. **Handle method calls (dot notation):**
```c
// In expression parsing, after primary expression
if (match(parser, TOKEN_DOT)) {
    // Get method name
    Token method = expect(parser, TOKEN_IDENTIFIER);
    
    // Check method names: readLine, readInt, readChar, etc.
    if (strcmp(method.value, "readLine") == 0) {
        generate_stream_readline(parser);
        return TYPE_STRING;
    } else if (strcmp(method.value, "readInt") == 0) {
        generate_stream_readint(parser);
        return TYPE_INT;
    }
    // ... etc for other methods
}
```

### File: `src/parser_codegen.c`

**Add new functions:**
```c
void generate_read_sysin(Parser* parser) {
    // Allocate Stream struct on stack (4KB+)
    // Initialize fd = 0 (stdin)
    // Initialize buffer, pos=0, buffered=0, eof=false
    // Leave struct pointer in result register
}

void generate_read_file(Parser* parser, const char* filename) {
    // Call sys_open internally (hidden from user)
    // Allocate Stream struct on stack
    // Initialize with returned fd
    // If fd < 0, set eof=true (error state)
}

void generate_stream_readline(Parser* parser) {
    // Access Stream struct from stack
    // Read buffered data until '\n'
    // Allocate string on heap
    // Return string pointer
}

void generate_stream_readint(Parser* parser) {
    // Read characters using buffer
    // Parse as integer
    // Return int value
}

void generate_stream_readchar(Parser* parser) {
    // Read single char from buffer
    // Return char value
}

void generate_stream_close(Parser* parser) {
    // Get fd from Stream
    // Call sys_close (hidden)
    // Mark stream as closed
}

void generate_stream_eof(Parser* parser) {
    // Access Stream struct
    // Return eof_flag value
}
```

### File: `src/parser_statements.c`

**Changes:**
- Remove direct access to `sys_read()`, `sys_open()`, `sys_close()`
- These become internal functions only, not accessible by user code

**Remove from built-in function list:**
```c
// DELETE these from is_builtin_function():
// strcmp(name, "sys_read") == 0 ||
// strcmp(name, "sys_open") == 0 ||
// strcmp(name, "sys_close") == 0 ||
```

---

## 🧪 Testing Strategy

### Test Suite Structure

Create new test files in `tests/`:

1. **`tests/stream_basic.dmm`** - Basic stream creation and constants
2. **`tests/stream_readline.dmm`** - Read lines from stdin
3. **`tests/stream_readint.dmm`** - Read integers from stdin
4. **`tests/stream_file_read.dmm`** - Read from files
5. **`tests/stream_file_lines.dmm`** - Read file line by line
6. **`tests/stream_eof.dmm`** - EOF detection
7. **`tests/stream_close.dmm`** - Closing streams
8. **`tests/stream_mixed.dmm`** - Mixed type reading

### Example Test 1: `tests/stream_basic.dmm`

```javascript
func main() -> void {
    var input:Stream = read(sysin);
    println("Stream created successfully");
}
```

**Test command:**
```bash
./test_stream_basic
# Expected output:
# Stream created successfully
```

### Example Test 2: `tests/stream_readline.dmm`

```javascript
func main() -> void {
    var input:Stream = read(sysin);
    println("Enter your name:");
    var name:string = input.readLine();
    println("Hello, " + name);
}
```

**Test command:**
```bash
echo "Alice" | ./test_stream_readline
# Expected output:
# Enter your name:
# Hello, Alice
```

### Example Test 3: `tests/stream_file_read.dmm`

```javascript
func main() -> void {
    // First create test file
    var out:Stream = write("test.txt");
    out.writeLine("Hello from file");
    out.close();
    
    // Now read it back
    var file:Stream = read("test.txt");
    var line:string = file.readLine();
    file.close();
    
    println(line);
}
```

**Expected output:**
```
Hello from file
```

### Test Automation

Update `run_tests.sh` to support input piping:

```bash
# For tests with input files
if [ -f "tests/input/$test_name.input" ]; then
    cat "tests/input/$test_name.input" | ./$test_executable > output.txt
else
    ./$test_executable > output.txt
fi
```

---

## 📚 Documentation Updates

### 1. Update `README.md`

Add section:
```markdown
### Stream-Based I/O

**Constants:**
- `sysin` - Standard input stream
- `sysout` - Standard output stream  
- `syserr` - Standard error stream

**Functions:**
- `read(sysin)` - Open stdin for reading
- `read(filename)` - Open file for reading

**Stream Methods:**
- `stream.readLine()` - Read entire line as string
- `stream.readInt()` - Read and parse integer
- `stream.readChar()` - Read single character
- `stream.readFloat()` - Read floating-point number
- `stream.close()` - Close stream
- `stream.eof()` - Check if end-of-file reached
```

### 2. Create `STREAM_API.md`

Comprehensive guide with:
- Stream type explanation
- All stream methods with signatures
- Code examples for each method
- File reading patterns
- Error handling with EOF
- Best practices

### 3. Update `examples/LANGUAGE_DOCUMENTATION.md`

Add "Stream-Based I/O" section with complete examples.

### 4. Create Example Programs

In `code_examples/`:
- `13_input_basics.dmm` - Basic console input with streams
- `14_file_reading.dmm` - Reading files line by line
- `15_interactive_menu.dmm` - Interactive CLI application
- `16_data_processor.dmm` - Processing data files
- `17_simple_grep.dmm` - File searching tool

---

## 🚀 Implementation Timeline

### Phase 1: Foundation (Days 1-3)
- ✅ Day 1: Add Stream type to type system
- ✅ Day 1: Add sysin/sysout/syserr constants to lexer
- ✅ Day 2: Implement `read(sysin)` function
- ✅ Day 3: Implement `read(filename)` function
- ✅ Day 3: Basic stream structure and initialization

### Phase 2: Stream Methods (Days 4-7)
- ✅ Day 4: Implement `stream.readLine()` method
- ✅ Day 5: Implement `stream.readInt()` method
- ✅ Day 6: Implement `stream.readChar()` method
- ✅ Day 7: Implement `stream.close()` and `stream.eof()`

### Phase 3: Additional Methods (Days 8-10)
- ✅ Day 8: Implement `stream.readFloat()` 
- ✅ Day 9: Implement `stream.readDouble()`
- ✅ Day 10: Method call parsing and dot notation

### Phase 4: Testing & Bug Fixes (Days 11-14)
- ✅ Day 11-12: Write comprehensive test suite
- ✅ Day 13: Integration testing with existing code
- ✅ Day 14: Bug fixes and edge cases

### Phase 5: Documentation (Days 15-17)
- ✅ Day 15: Write STREAM_API.md
- ✅ Day 16: Create example programs
- ✅ Day 17: Update main README and language docs

---

## 🎯 Success Criteria

### Minimum Viable Product (MVP)

- ✅ Stream type added to compiler
- ✅ `sysin`, `sysout`, `syserr` constants recognized
- ✅ `read(sysin)` returns working Stream
- ✅ `read("file.txt")` returns working Stream
- ✅ `stream.readLine()` reads full lines
- ✅ `stream.readInt()` parses integers correctly
- ✅ `stream.close()` closes files properly
- ✅ Basic tests pass
- ✅ Documentation updated

### Full Release

- ✅ All stream methods implemented (readChar, readFloat, readDouble)
- ✅ `stream.eof()` detection works correctly
- ✅ Method call syntax (dot notation) fully functional
- ✅ No direct syscall access for users (hidden implementation)
- ✅ Comprehensive test suite (8+ tests)
- ✅ Example programs demonstrate all features
- ✅ STREAM_API.md documentation complete
- ✅ Integration with existing codebase seamless

---

## 🐛 Known Challenges & Solutions

### Challenge 1: Stream Object Size

**Problem:** Stream struct is large (~4KB) due to internal buffer. Storing on stack could cause overflow.

**Solution:** 
- Allocate Stream on stack but carefully manage stack space
- Alternatively, use heap allocation for Stream objects
- Document size limitations for deeply nested function calls

### Challenge 2: Method Call Syntax

**Problem:** Parser doesn't currently support method calls with dot notation.

**Solution:**
- Extend expression parser to handle DOT token after primary expression
- Generate assembly that passes Stream pointer as implicit first argument
- Similar to how structs methods work currently

### Challenge 3: Hiding Syscalls

**Problem:** Need to prevent user code from calling sys_read, sys_open, etc.

**Solution:**
- Remove these from `is_builtin_function()` check
- Keep implementation code but make them internal-only
- Generate errors if user tries to call them

### Challenge 4: Float Parsing

**Problem:** Converting ASCII to float is complex.

**Solution:**
- Implement character-by-character parsing in assembly
- Handle decimal point, negative numbers
- Use x86 SSE instructions for float conversion
- Start with basic implementation, enhance later

### Challenge 5: File Error Handling

**Problem:** Opening non-existent files should not crash.

**Solution:**
- Check file descriptor after open (< 0 = error)
- Set Stream.eof_flag = true on error
- User can check with stream.eof() before reading

---

## 🔗 Related Files and Resources

### Source Files to Modify
- `src/compiler_types.h` - Add TYPE_STREAM and StreamState struct
- `src/lexer.c` - Add sysin/sysout/syserr keywords
- `src/parser_types.c` - Stream type registration, remove syscall access
- `src/parser_expressions.c` - Method call parsing, read() function
- `src/parser_codegen.c` - Assembly generation for streams
- `src/parser_statements.c` - Remove sys_* from user-accessible functions

### Documentation Files to Create/Update
- `STREAM_API.md` - New comprehensive stream API guide (to create)
- `README.md` - Update with stream-based I/O section
- `examples/LANGUAGE_DOCUMENTATION.md` - Add stream examples

### Test Files
- `tests/stream_*.dmm` - New stream tests (to create)
- `run_tests.sh` - Test runner (to update for piped input)

### Old Files to Deprecate/Remove
- `examples/IO_SYSTEM_DOCUMENTATION.md` - Old syscall-based docs
- `examples/READ_FUNCTION_GUIDE.md` - Old format-based read() docs
- Remove sys_* examples from documentation

---

## 📊 Risk Assessment

| Risk | Severity | Probability | Mitigation |
|------|----------|-------------|------------|
| Stack overflow from large Stream objects | High | Medium | Careful stack management, document limitations |
| Method call parsing complexity | Medium | Medium | Reuse existing struct method infrastructure |
| Breaking existing sys_* code | High | High | This is intentional - deprecate old API |
| File reading edge cases | Medium | Low | Comprehensive testing with various file types |
| Performance with buffering | Low | Low | 4KB buffer should be efficient for most cases |

---

## 🎓 Learning Resources

### For Implementers

1. **Linux Syscalls**: `man 2 read`, `man 2 open`
2. **x86-64 Assembly**: System V ABI documentation
3. **Float Parsing**: Algorithms in "Computer Arithmetic" by Behrooz Parhami
4. **Buffer Management**: "The C Programming Language" by K&R

### For Users

1. Standard library documentation (stdlib/io.txt)
2. Example programs in code_examples/
3. Language documentation (examples/LANGUAGE_DOCUMENTATION.md)

---

## 🏁 Conclusion

This implementation plan provides a clean, object-oriented approach to input reading in the DMM compiler. The design philosophy emphasizes:

1. **Clean API** - No syscall exposure, high-level abstractions only
2. **Stream-Based** - Unified interface for console and file I/O
3. **Object-Oriented** - Methods on Stream objects (dot notation)
4. **Type-Safe** - Compiler-enforced Stream type
5. **User-Friendly** - Simple, intuitive API for developers

### Key Design Decisions

✅ **Stream objects** instead of raw file descriptors  
✅ **Constants** (sysin, sysout, syserr) instead of magic numbers  
✅ **Methods** (stream.readLine()) instead of functions  
✅ **Hidden syscalls** - implementation detail, not user-facing  
✅ **Consistent API** - same methods work for stdin and files  

### Breaking Changes

⚠️ This is a **breaking change** from existing I/O system:
- Old: `read(0, "%i")` format-based approach
- New: `read(sysin).readInt()` stream-based approach
- Old: Direct syscall access (`sys_read`, etc.)
- New: Hidden implementation, clean API only

**Migration Path:** Update existing code to use new Stream API, remove direct syscall usage.

### Next Steps

1. ✅ Review and approve this plan
2. Create feature branch: `git checkout -b input-reading`
3. Begin Phase 1: Add Stream type and constants
4. Implement incrementally with tests at each phase
5. Update documentation as features are completed

---

**Plan Version**: 2.0 (Stream-Based Design)  
**Last Updated**: 2025-12-27  
**Status**: Ready for Implementation ✅  
**Breaking Changes**: Yes - New API replaces old syscall-based approach
