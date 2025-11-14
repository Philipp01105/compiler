# read() Function Guide

## Overview

The `read()` function provides a unified, scanf-like interface for reading formatted input from streams without using any C library functions.

## Syntax

```javascript
var value:type = read(stream, format_specifier);
```

## Parameters

- **stream** (int): File descriptor
  - `0` for stdin
  - `System.in.fd` for stdin (using enum)
  - File descriptor from `open_read()`

- **format** (string literal): Format specifier
  - `"%i"` or `"%d"` - Read integer
  - `"%c"` - Read single character

## Return Value

Returns a value of the type specified by the format:
- Integer for `"%i"` or `"%d"`
- Character for `"%c"`

## Implementation

The `read()` function is a **compiler built-in** that:
1. Generates direct Linux syscall code
2. Parses input based on format specifier
3. Returns typed values
4. Has zero function call overhead

## Usage Examples

### Example 1: Basic Integer Reading

```javascript
func main() -> void {
    println("Enter a number:");
    var num:int = read(0, "%i");
    println(num);
}
```

**Run:**
```bash
$ echo "42" | ./program
Enter a number:
42
```

### Example 2: Using System.in Enum

```javascript
#import <stdlib/io.txt>

func main() -> void {
    println("Enter a number:");
    var num:int = read(System.in.fd, "%i");
    print("You entered: ");
    println(num);
}
```

### Example 3: Simple Calculator

```javascript
#import <stdlib/io.txt>

func main() -> void {
    println("Enter two numbers:");
    
    var a:int = read(0, "%i");
    var b:int = read(0, "%i");
    
    var sum:int = a + b;
    print("Sum: ");
    println(sum);
}
```

**Run:**
```bash
$ echo -e "10\n20" | ./calculator
Enter two numbers:
Sum: 30
```

### Example 4: Reading from a File

```javascript
#import <stdlib/io.txt>

func main() -> void {
    // Create a file with test data
    write_file("numbers.txt", "100\n200\n300\n");
    
    // Open and read
    var fd:int = open_read("numbers.txt");
    
    var n1:int = read(fd, "%i");
    var n2:int = read(fd, "%i");
    var n3:int = read(fd, "%i");
    
    close_file(fd);
    
    // Print sum
    println(n1 + n2 + n3);  // Prints: 600
}
```

### Example 5: Character Reading

```javascript
func main() -> void {
    println("Enter a character:");
    var c:char = read(0, "%c");
    print("You entered: ");
    println(c);
}
```

## Comparison with Other Languages

### C (scanf)
```c
int num;
scanf("%d", &num);
```

### Java (Scanner)
```java
Scanner scanner = new Scanner(System.in);
int num = scanner.nextInt();
```

### Go (fmt.Scan)
```go
var num int
fmt.Scan(&num)
```

### DMM (read)
```javascript
var num:int = read(System.in.fd, "%i");
```

## Format Specifiers

| Format | Type | Description | Example |
|--------|------|-------------|---------|
| `"%i"` | int | Read integer (base 10) | `read(0, "%i")` |
| `"%d"` | int | Read integer (same as %i) | `read(0, "%d")` |
| `"%c"` | char | Read single character | `read(0, "%c")` |

## Technical Details

### Generated Assembly

For `read(0, "%i")`, the compiler generates:

```asm
# Allocate buffer
subq $32, %rsp
movq %rsp, %r15

# System call: read(0, buffer, 31)
popq %rdi              # fd = 0
movq %r15, %rsi        # buffer
movq $31, %rdx         # count
movq $0, %rax          # syscall: read
syscall

# Parse integer from string
# ... (digit parsing loop)

# Clean up and return
addq $32, %rsp
pushq %rax             # result
```

### Performance

- **Zero overhead**: Direct syscall, no library calls
- **Type-safe**: Returns correct type based on format
- **Efficient**: Minimal stack usage
- **Pure**: No C library dependencies

## Limitations

- Format string must be a string literal (compile-time constant)
- String reading not directly supported - use `read_string()` instead
- Currently supports only `%i`, `%d`, and `%c` formats

## Error Handling

- Invalid format specifiers cause compile-time errors
- Read failures return last successfully parsed value
- File descriptor errors handled by underlying syscall

## Best Practices

1. **Use System.in for readability:**
   ```javascript
   var num:int = read(System.in.fd, "%i");
   ```

2. **Always check file descriptors:**
   ```javascript
   var fd:int = open_read("file.txt");
   if (fd >= 0) {
       var value:int = read(fd, "%i");
       close_file(fd);
   }
   ```

3. **Close files after reading:**
   ```javascript
   var fd:int = open_read("data.txt");
   var value:int = read(fd, "%i");
   close_file(fd);  // Always close!
   ```

## See Also

- [IO_SYSTEM_DOCUMENTATION.md](IO_SYSTEM_DOCUMENTATION.md) - Complete I/O system documentation
- [stdlib/io.txt](stdlib/io.txt) - Standard library I/O module
- [examples/read_demo.dmm](examples/read_demo.dmm) - Basic demonstrations
- [examples/read_function_guide.dmm](examples/read_function_guide.dmm) - Comprehensive examples

## Implementation Commits

- c54e53d - Initial read() implementation
- 0e8fd79 - Documentation updates
- e471a26 - Comprehensive examples and tests
