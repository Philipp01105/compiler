# DMM I/O System Documentation

## Overview

The DMM compiler now includes a complete I/O system that operates **without any C library dependencies**. All I/O operations use direct Linux syscalls, inspired by how Java and Go handle low-level I/O.

## Architecture

The I/O system has three layers:

### 1. Compiler Built-ins (Lowest Level)
These functions are recognized by the compiler and generate direct syscall assembly code:

- **`sys_write(fd:int, buffer:string, count:int) -> int`**
  - Linux syscall: write (1)
  - Writes data to a file descriptor
  - Returns: bytes written or -1 on error

- **`sys_read(fd:int, buffer:string, count:int) -> int`**
  - Linux syscall: read (0)
  - Reads data from a file descriptor
  - Returns: bytes read or -1 on error

- **`sys_open(pathname:string, flags:int, mode:int) -> int`**
  - Linux syscall: open (2)
  - Opens/creates a file
  - Returns: file descriptor or -1 on error

- **`sys_close(fd:int) -> int`**
  - Linux syscall: close (3)
  - Closes a file descriptor
  - Returns: 0 on success, -1 on error

- **`io_strlen(s:string) -> int`**
  - Calculates string length (inline code generation)
  - Returns: number of characters until null terminator

- **`io_int_to_str(value:int, buffer:string, buffer_size:int) -> int`**
  - Converts integer to ASCII string
  - Returns: length of resulting string

- **`io_str_to_int(s:string) -> int`**
  - Parses ASCII string to integer
  - Returns: integer value

- **`read(stream:int, format:string) -> int|char`** ⭐ NEW
  - Unified input function with format specifiers (like scanf/Java Scanner)
  - Parameters:
    - `stream`: File descriptor (0 for stdin, or use `System.in.fd`)
    - `format`: Format specifier - `"%i"` or `"%d"` for int, `"%c"` for char
  - Returns: Value based on format specifier
  - Examples:
    - `var num:int = read(System.in.fd, "%i");`
    - `var c:char = read(0, "%c");`
    - `var fd:int = open_read("input.txt"); var value:int = read(fd, "%i");`

### 2. Standard Library (High Level)
Location: `stdlib/io.txt`

Import with: `#import <stdlib/io.txt>`

**File Operations:**
- `write_file(filename:string, content:string) -> int` - Write entire file
- `read_file(filename:string, buffer:string, max_size:int) -> int` - Read entire file
- `append_file(filename:string, content:string) -> int` - Append to file
- `file_exists(filename:string) -> int` - Check if file exists

**Stream Operations:**
- `open_read(path:string) -> int` - Open for reading
- `open_write(path:string) -> int` - Open for writing (create/truncate)
- `open_append(path:string) -> int` - Open for appending
- `write_str_to(fd:int, s:string) -> int` - Write string to stream
- `read_from(fd:int, buffer:string, count:int) -> int` - Read from stream
- `close_file(fd:int) -> int` - Close stream

**System Stream Constants:**
- `System.in.fd` - stdin (file descriptor 0)
- `System.out.fd` - stdout (file descriptor 1)
- `System.err.fd` - stderr (file descriptor 2)

**Input Operations:**
- `read(stream:int, format:string) -> int|char` - Unified input with format specifiers ⭐
- `read_string(stream:int, buffer:string, max_size:int) -> int` - Read string/line

**Utility Functions:**
- `strlen(s:string) -> int` - Get string length
- `int_to_string(value:int, buffer:string, buffer_size:int) -> int` - Convert int to string
- `string_to_int(s:string) -> int` - Convert string to int

### 3. Built-in Print Functions
The compiler provides built-in `print()` and `println()` functions that work with all types. These are the ONLY print functions needed - do not use print_int, print_char, etc.

## Usage Examples

### Example 1: Using read() Function (NEW)
```javascript
#import <stdlib/io.txt>

func main() -> void {
    println("Enter a number:");
    var num:int = read(System.in.fd, "%i");
    
    print("You entered: ");
    println(num);
}
```

### Example 2: Direct Syscalls
```javascript
func main() -> void {
    var msg:string = "Hello, syscalls!\n";
    var len:int = io_strlen(msg);
    sys_write(1, msg, len);  // Write to stdout
}
```

### Example 3: Reading from Files with read()
```javascript
#import <stdlib/io.txt>

func main() -> void {
    // Write number to file
    write_file("data.txt", "42\n");
    
    // Open and read using read()
    var fd:int = open_read("data.txt");
    var value:int = read(fd, "%i");
    close_file(fd);
    
    println(value);  // Prints: 42
}
```

### Example 4: High-Level File Operations
```javascript
#import <stdlib/io.txt>

func main() -> void {
    // Write to file
    write_file("output.txt", "Hello, file!\n");
    
    // Read from file
    var[1024] buffer:char;
    var size:int = read_file("output.txt", &buffer, 1023);
    
    // Check if file exists
    if (file_exists("output.txt") == 1) {
        println("File exists!");
    }
}
```

### Example 5: Stream-Based I/O
```javascript
#import <stdlib/io.txt>

func main() -> void {
    var fd:int = open_write("log.txt");
    write_str_to(fd, "Log entry 1\n");
    write_str_to(fd, "Log entry 2\n");
    close_file(fd);
}
```

### Example 6: Calculator with read()
```javascript
#import <stdlib/io.txt>

func main() -> void {
    println("Enter first number:");
    var a:int = read(System.in.fd, "%i");
    
    println("Enter second number:");
    var b:int = read(System.in.fd, "%i");
    
    print("Sum: ");
    println(a + b);
}
```

## File Descriptors

- `0` = stdin (standard input)
- `1` = stdout (standard output)
- `2` = stderr (standard error)

## File Open Flags

Common flag combinations for `sys_open()`:
- `0` = O_RDONLY (read only)
- `1` = O_WRONLY (write only)
- `2` = O_RDWR (read/write)
- `577` = O_WRONLY | O_CREAT | O_TRUNC (write, create, truncate)
- `1089` = O_WRONLY | O_CREAT | O_APPEND (write, create, append)

Common mode: `420` (octal 0644 = rw-r--r--)

## Benefits

1. **No C Library Dependencies**: Programs can run with only the Linux kernel
2. **Full Control**: Direct access to syscall parameters and return values
3. **Predictable Behavior**: No hidden C library behavior
4. **Minimal Overhead**: Direct syscall instructions, no library call overhead
5. **Educational**: Understand how I/O actually works at the OS level

## Implementation Details

All syscall functions generate inline assembly code:

```assembly
# Example: sys_write(1, "Hello", 5)
movq $1, %rax      # syscall number: write
movq $1, %rdi      # fd: stdout
movq $msg, %rsi    # buffer: string pointer
movq $5, %rdx      # count: 5 bytes
syscall            # invoke kernel
```

No function call overhead - the syscall instruction goes directly to the kernel.

## Testing

Run the I/O system tests:
```bash
./build/compiler tests/io_syscall_direct.dmm
gcc -no-pie tests/io_syscall_direct.dmm.s -o tests/io_syscall_direct
./tests/io_syscall_direct
```

Or run the comprehensive demo:
```bash
./build/compiler examples/io_demo.dmm
gcc -no-pie examples/io_demo.dmm.s -o examples/io_demo
./examples/io_demo
```

## Future Enhancements

Potential additions:
- Network I/O (socket syscalls)
- Directory operations (opendir, readdir)
- File metadata operations (stat, fstat)
- Pipe operations
- Memory-mapped I/O

All following the same principle: pure syscalls, no C library dependencies.

## Conclusion

The DMM I/O system provides a complete, C-library-free I/O solution that demonstrates how modern system programming can directly interface with the operating system kernel. It's educational, efficient, and provides full control over I/O operations.
