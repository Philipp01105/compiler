# DMM Standard Library

This directory contains the DMM standard library modules.

## I/O Module (io.txt)

The I/O module provides syscall-based input/output operations without using any C library functions.

### Built-in Syscall Functions

These functions are implemented directly by the compiler and generate Linux syscall assembly code:

#### Core Syscalls

- `sys_write(fd:int, buffer:string, count:int) -> int`
  - Write data to a file descriptor
  - Returns: number of bytes written, or -1 on error
  - Linux syscall: write (1)

- `sys_read(fd:int, buffer:string, count:int) -> int`
  - Read data from a file descriptor
  - Returns: number of bytes read, or -1 on error
  - Linux syscall: read (0)

- `sys_open(pathname:string, flags:int, mode:int) -> int`
  - Open a file
  - Returns: file descriptor, or -1 on error
  - Linux syscall: open (2)
  - Common flags:
    - 0 = O_RDONLY (read only)
    - 1 = O_WRONLY (write only)
    - 2 = O_RDWR (read/write)
    - 64 = O_CREAT (create if doesn't exist)
    - 512 = O_TRUNC (truncate to zero length)
    - 1024 = O_APPEND (append mode)
    - 577 = O_WRONLY | O_CREAT | O_TRUNC (common for writing)
  - Common mode: 420 (octal 0644, rw-r--r--)

- `sys_close(fd:int) -> int`
  - Close a file descriptor
  - Returns: 0 on success, or -1 on error
  - Linux syscall: close (3)

#### Helper Functions

- `io_strlen(s:string) -> int`
  - Calculate string length (counts characters until null terminator)
  - Returns: length of string

- `io_int_to_str(value:int, buffer:string, buffer_size:int) -> int`
  - Convert integer to ASCII string
  - Returns: length of resulting string

- `io_str_to_int(s:string) -> int`
  - Parse ASCII string to integer
  - Returns: integer value

### File Descriptors

- 0 = stdin (standard input)
- 1 = stdout (standard output)
- 2 = stderr (standard error)

### Usage Examples

#### Write to stdout

```javascript
func main() -> void {
    var msg:string = "Hello, World!\n";
    var len:int = io_strlen(msg);
    sys_write(1, msg, len);
}
```

#### Read from stdin

```javascript
func main() -> void {
    var[100] buffer:char;
    var bytes_read:int = sys_read(0, &buffer, 99);
    sys_write(1, &buffer, bytes_read);
}
```

#### Write to a file

```javascript
func main() -> void {
    var filename:string = "output.txt";
    var content:string = "Hello, file!\n";
    
    // Open file for writing (create/truncate)
    var fd:int = sys_open(filename, 577, 420);
    
    // Write content
    var len:int = io_strlen(content);
    sys_write(fd, content, len);
    
    // Close file
    sys_close(fd);
}
```

#### Read from a file

```javascript
func main() -> void {
    var filename:string = "input.txt";
    var[1024] buffer:char;
    
    // Open file for reading
    var fd:int = sys_open(filename, 0, 0);
    
    // Read content
    var bytes_read:int = sys_read(fd, &buffer, 1023);
    
    // Write to stdout
    sys_write(1, &buffer, bytes_read);
    
    // Close file
    sys_close(fd);
}
```

## Implementation Details

All syscall I/O functions are implemented as compiler built-ins that generate direct assembly code for Linux syscalls. This means:

1. **No C library dependencies**: The generated code uses only Linux kernel syscalls
2. **Maximum efficiency**: No function call overhead, direct syscall instructions
3. **Full control**: Direct access to all Linux syscall features
4. **Portability**: Works on any Linux system without external dependencies

The implementation is inspired by how Java and Go handle I/O at a low level, providing both raw syscall access and higher-level convenience functions.

## Future Enhancements

The stdlib can be extended with additional modules for:
- Math operations
- String manipulation
- Array utilities
- Memory management
- Network operations

All following the same principle of avoiding C library dependencies where possible.
