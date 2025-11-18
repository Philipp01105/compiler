# DMM Standard Library - I/O Module

## Overview

The `io` module provides platform-independent I/O operations for file handling and standard input operations. It works on Linux and compiles to x86-64 assembly.

## Import

```javascript
#import <stdlib/io.txt>
```

## Required Functions (as per issue specification)

### File Operations

#### `read_file(filename:string, buffer:string, max_size:int) -> int`

Reads entire file content into a buffer.

**Parameters:**
- `filename`: Path to the file to read
- `buffer`: Character array to store the content
- `max_size`: Maximum bytes to read

**Returns:**
- Number of bytes read on success
- `-1` on error (file not found, permission denied, etc.)

**Example:**
```javascript
var[1024] buffer:char;
var bytes:int = read_file("data.txt", &buffer, 1023);
if (bytes > 0) {
    println("File read successfully");
}
```

#### `write_file(path:string, content:string) -> int`

Writes content to a file (creates new or overwrites existing).

**Parameters:**
- `path`: Path to the file to write
- `content`: String content to write

**Returns:**
- `1` on success
- `0` on error

**Example:**
```javascript
var result:int = write_file("output.txt", "Hello, World!\n");
if (result == 1) {
    println("File written successfully");
}
```

#### `append_file(path:string, content:string) -> int`

Appends content to a file (creates if doesn't exist).

**Parameters:**
- `path`: Path to the file to append to
- `content`: String content to append

**Returns:**
- `1` on success
- `0` on error

**Example:**
```javascript
append_file("log.txt", "Log entry 1\n");
append_file("log.txt", "Log entry 2\n");
append_file("log.txt", "Log entry 3\n");
```

#### `file_exists(path:string) -> int`

Checks if a file exists.

**Parameters:**
- `path`: Path to the file to check

**Returns:**
- `1` if file exists
- `0` if file doesn't exist

**Example:**
```javascript
if (file_exists("config.txt") == 1) {
    println("Config file found");
} else {
    println("Config file not found");
}
```

#### `read_stdin() -> int`

Reads from standard input.

**Returns:**
- Number of bytes read from stdin

**Note:** For more advanced input with format specifiers, use the `read()` built-in function.

**Example:**
```javascript
var bytes:int = read_stdin();
println("Read bytes from stdin");
```

## Additional Helper Functions

### Stream-Based I/O

#### `open_read(path:string) -> int`

Opens a file for reading.

**Returns:** File descriptor on success, `-1` on error

#### `open_write(path:string) -> int`

Opens a file for writing (creates or truncates).

**Returns:** File descriptor on success, `-1` on error

#### `open_append(path:string) -> int`

Opens a file for appending.

**Returns:** File descriptor on success, `-1` on error

#### `write_str_to(fd:int, s:string) -> int`

Writes a string to a file descriptor.

**Returns:** Number of bytes written, `-1` on error

#### `read_from(fd:int, buffer:string, count:int) -> int`

Reads from a file descriptor into a buffer.

**Returns:** Number of bytes read, `-1` on error

#### `close_file(fd:int) -> int`

Closes a file descriptor.

**Returns:** `0` on success, `-1` on error

**Example:**
```javascript
var fd:int = open_write("output.txt");
if (fd >= 0) {
    write_str_to(fd, "Line 1\n");
    write_str_to(fd, "Line 2\n");
    close_file(fd);
}
```

### System Stream Constants

The module provides an enum for standard stream file descriptors:

```javascript
System.in.fd    // stdin  (value: 0)
System.out.fd   // stdout (value: 1)
System.err.fd   // stderr (value: 2)
```

**Example:**
```javascript
var num:int = read(System.in.fd, "%i");
```

### String Utilities

#### `strlen(s:string) -> int`

Calculates the length of a string (alias for `io_strlen`).

#### `read_string(stream:int, buffer:string, max_size:int) -> int`

Reads a string from a stream into a buffer.

**Returns:** Number of bytes read

## Compiler Built-ins

The following functions are built into the compiler and can be called directly (no wrapper needed):

- `io_strlen(s:string) -> int` - String length
- `io_int_to_str(value:int, buffer:string, buffer_size:int) -> int` - Convert int to string
- `io_str_to_int(s:string) -> int` - Convert string to int
- `read(stream:int, format:string) -> int|char` - Formatted input (like scanf)

### Using `read()` for Input

The `read()` function provides formatted input with format specifiers:

```javascript
// Read integer
var num:int = read(System.in.fd, "%i");

// Read character
var c:char = read(System.in.fd, "%c");

// Read from file
var fd:int = open_read("numbers.txt");
var value:int = read(fd, "%i");
close_file(fd);
```

## System Calls

All I/O operations in this module use direct Linux syscalls without C library dependencies:

- `sys_write(fd:int, buffer:string, count:int) -> int` - Write syscall
- `sys_read(fd:int, buffer:string, count:int) -> int` - Read syscall
- `sys_open(pathname:string, flags:int, mode:int) -> int` - Open syscall
- `sys_close(fd:int) -> int` - Close syscall

These are available as compiler built-ins if you need lower-level control.

## Platform Independence

The I/O module provides a uniform API that works identically on Linux and Windows (when compiled to x86-64 assembly). All platform-specific details are handled internally.

## File Open Flags (for advanced use)

If using `sys_open` directly:

- `0` = O_RDONLY (read only)
- `577` = O_WRONLY | O_CREAT | O_TRUNC (write, create, truncate)
- `1089` = O_WRONLY | O_CREAT | O_APPEND (write, create, append)

Default mode: `420` (octal 0644 = rw-r--r--)

## Complete Example

```javascript
#import <stdlib/io.txt>

func main() -> void {
    // Write to file
    println("Writing to file...");
    var result:int = write_file("test.txt", "Hello from DMM!\n");
    
    if (result == 1) {
        println("Write successful");
    }
    
    // Check if file exists
    if (file_exists("test.txt") == 1) {
        println("File exists");
        
        // Read the file
        var[256] buffer:char;
        var bytes:int = read_file("test.txt", &buffer, 255);
        
        if (bytes > 0) {
            println("File read successfully");
        }
    }
    
    // Append to file
    append_file("test.txt", "Second line\n");
    append_file("test.txt", "Third line\n");
    
    println("All operations completed");
}
```

## Testing

Run the I/O module test:

```bash
./build/compiler tests/io_stdlib_test.dmm
gcc -no-pie tests/io_stdlib_test.dmm.s -o tests/io_stdlib_test
./tests/io_stdlib_test
```

Or run all tests:

```bash
./run_tests.sh
```

## Version

I/O Module Version: 1.0.0
Compiler Version: 5.1.0+
Date: November 2025
