# String Features - Full Implementation

## Overview

This document describes the complete string implementation in the compiler, including string manipulation, heap allocation, and mutable strings.

## Table of Contents

1. [String Basics](#string-basics)
2. [String Indexing](#string-indexing)
3. [Built-in String Functions](#built-in-string-functions)
4. [Heap-Allocated Mutable Strings](#heap-allocated-mutable-strings)
5. [Examples](#examples)
6. [Limitations](#limitations)

---

## String Basics

### String Declaration

Strings are 64-bit pointers to character arrays:

```c
var name:string = "Hello";
var empty:string = "";
var message:string;  // Initialized with null pointer
```

### String Literals

String literals are stored in the read-only `.rdata` section:

```c
var greeting:string = "Hello World";
print("Direct: " + "Hello");
```

**Important:** String literals are read-only and cannot be modified.

---

## String Indexing

### Reading Characters

Access individual characters using array notation:

```c
var str:string = "Hello";
var first:char = str[0];   // 'H'
var second:char = str[1];  // 'e'
var last:char = str[4];    // 'o'

// String literals can also be indexed
var ch:char = "World"[0];  // 'W'
```

### Writing Characters (Mutable Strings)

Modify individual characters in heap-allocated strings:

```c
var mutable:string = strdup("Hello");
mutable[0] = 'J';  // Now "Jello"
mutable[1] = 'a';
mutable[2] = 'm';
mutable[3] = 'e';
mutable[4] = 's';  // Now "James"

free(mutable);
```

**Warning:** Writing to string literals (in `.rdata`) causes a segmentation fault.

---

## Built-in String Functions

The compiler provides several built-in string manipulation functions that call the standard C library.

### 1. strlen() - String Length

Returns the length of a string (number of characters before null terminator).

**Signature:** `strlen(str:string) -> int`

**Example:**
```c
var text:string = "Hello";
var len:int = strlen(text);  // 5

if (strlen("") == 0) {
    print("Empty string");
}

var last_index:int = strlen(text) - 1;
var last_char:char = text[last_index];  // 'o'
```

### 2. strcmp() - String Comparison

Compares two strings lexicographically.

**Signature:** `strcmp(str1:string, str2:string) -> int`

**Returns:**
- `0` if strings are equal
- `< 0` if str1 comes before str2
- `> 0` if str1 comes after str2

**Example:**
```c
var result:int = strcmp("Hello", "Hello");  // 0 (equal)

if (strcmp("Apple", "Banana") < 0) {
    print("Apple comes before Banana");
}

if (strcmp("World", "Hello") > 0) {
    print("World comes after Hello");
}

var str1:string = "Test";
var str2:string = "Test";
if (strcmp(str1, str2) == 0) {
    print("Strings are equal");
}
```

### 3. strcpy() - String Copy

Copies a string from source to destination.

**Signature:** `strcpy(dst:string, src:string) -> string`

**Returns:** The destination pointer

**Example:**
```c
var buffer:string = malloc(100);
strcpy(buffer, "Hello World");
print(buffer);  // "Hello World"

var src:string = "Copy Me";
strcpy(buffer, src);
print(buffer);  // "Copy Me"

free(buffer);
```

**Warning:** Destination must have enough space for the source string plus null terminator.

### 4. strcat() - String Concatenation

Appends source string to destination string.

**Signature:** `strcat(dst:string, src:string) -> string`

**Returns:** The destination pointer

**Example:**
```c
var buffer:string = malloc(100);
strcpy(buffer, "Hello");
strcat(buffer, " ");
strcat(buffer, "World");
print(buffer);  // "Hello World"

free(buffer);
```

**Warning:** 
- Destination must have enough space for both strings plus null terminator
- Destination is modified in place

### 5. strdup() - Duplicate String

Creates a heap-allocated copy of a string.

**Signature:** `strdup(str:string) -> string`

**Returns:** Pointer to new string on heap

**Example:**
```c
var original:string = "Original";
var copy:string = strdup(original);

// Modify the copy
copy[0] = 'M';
copy[1] = 'u';

print("Original: " + original);  // "Original"
print("Modified: " + copy);      // "Mudified"

free(copy);
```

**Memory:** Always remember to `free()` the duplicated string.

---

## Heap-Allocated Mutable Strings

To create strings that can be modified, allocate them on the heap.

### Memory Management Functions

#### malloc() - Allocate Memory

Allocates a block of memory on the heap.

**Signature:** `malloc(size:int) -> string`

**Example:**
```c
var buffer:string = malloc(100);  // Allocate 100 bytes

// Initialize the buffer
strcpy(buffer, "Hello");

// Modify it
buffer[0] = 'J';

// Clean up
free(buffer);
```

#### free() - Free Memory

Frees previously allocated memory.

**Signature:** `free(ptr:string) -> void`

**Example:**
```c
var str:string = malloc(50);
strcpy(str, "Temporary");
// ... use str ...
free(str);
```

**Warning:** Always free allocated memory to prevent memory leaks.

### Mutable String Workflow

1. **Allocate:** Create buffer with `malloc()` or `strdup()`
2. **Initialize:** Use `strcpy()` to set initial value
3. **Modify:** Change characters using index notation
4. **Clean up:** Free with `free()`

---

## Examples

### Example 1: Basic String Manipulation

```c
func main() -> void {
    var str:string = "Hello World";
    
    // Get length
    var len:int = strlen(str);
    print("Length: " + len);  // 11
    
    // Access characters
    var first:char = str[0];
    var last:char = str[len - 1];
    print("First: " + first);  // 'H'
    print("Last: " + last);    // 'd'
}
```

### Example 2: String Comparison

```c
func checkPassword(input:string, correct:string) -> bit {
    if (strcmp(input, correct) == 0) {
        return 1;  // Match
    } else {
        return 0;  // No match
    }
}

func main() -> void {
    if (checkPassword("secret", "secret") == 1) {
        print("Access granted");
    }
}
```

### Example 3: Mutable String Building

```c
func main() -> void {
    // Create a mutable buffer
    var buffer:string = malloc(100);
    
    // Build string character by character
    buffer[0] = 'H';
    buffer[1] = 'e';
    buffer[2] = 'l';
    buffer[3] = 'l';
    buffer[4] = 'o';
    buffer[5] = '\0';  // Null terminator
    
    print(buffer);  // "Hello"
    
    free(buffer);
}
```

### Example 4: String Concatenation

```c
func buildGreeting(name:string) -> string {
    var result:string = malloc(200);
    
    strcpy(result, "Hello, ");
    strcat(result, name);
    strcat(result, "!");
    
    return result;
}

func main() -> void {
    var greeting:string = buildGreeting("Alice");
    print(greeting);  // "Hello, Alice!"
    free(greeting);
}
```

### Example 5: String Transformation

```c
func toUpperFirst(str:string) -> string {
    var result:string = strdup(str);
    
    // Convert first character to uppercase (simple ASCII)
    if (result[0] >= 'a' && result[0] <= 'z') {
        var diff:int = 'a' - 'A';
        var old:char = result[0];
        var new:char = old - diff;
        result[0] = new;
    }
    
    return result;
}

func main() -> void {
    var lower:string = "hello";
    var upper:string = toUpperFirst(lower);
    
    print("Original: " + lower);  // "hello"
    print("Modified: " + upper);  // "Hello"
    
    free(upper);
}
```

### Example 6: String Array Processing

```c
func countChar(str:string, target:char) -> int {
    var count:int = 0;
    var len:int = strlen(str);
    
    for (var i:int = 0; i < len; i++) {
        if (str[i] == target) {
            var temp:int = count + 1;
            count = temp;
        }
    }
    
    return count;
}

func main() -> void {
    var text:string = "Hello World";
    var l_count:int = countChar(text, 'l');
    var o_count:int = countChar(text, 'o');
    
    print("'l' appears " + l_count + " times");  // 3
    print("'o' appears " + o_count + " times");  // 2
}
```

---

## Limitations

### 1. No Multi-dimensional String Arrays

Currently not supported:
```c
var names[3]:string;  // NOT SUPPORTED
```

### 2. No String Literals with Escape Sequences (Limited)

Some escape sequences work, others may not:
```c
var str:string = "Line1\nLine2";  // \n works
var tab:string = "Col1\tCol2";    // \t works
```

### 3. No Automatic Bounds Checking

The compiler doesn't check if string operations overflow:
```c
var small:string = malloc(5);
strcpy(small, "This is too long");  // UNSAFE! Buffer overflow
```

Always ensure buffers are large enough.

### 4. No String Pooling Optimization

Duplicate string literals create separate entries in `.rdata`:
```c
var s1:string = "Hello";
var s2:string = "Hello";
// s1 and s2 may point to different locations
```

### 5. Manual Memory Management

No garbage collection - you must manually free allocated strings:
```c
var str:string = strdup("Temp");
// ... use str ...
free(str);  // Don't forget!
```

### 6. String Literals are Read-Only

Cannot modify string literals:
```c
var lit:string = "Hello";
lit[0] = 'J';  // SEGFAULT! String literal in .rdata
```

Use `strdup()` or `malloc()` to create mutable copies.

---

## Best Practices

1. **Always free heap-allocated strings:**
   ```c
   var str:string = strdup("Text");
   // ... use str ...
   free(str);
   ```

2. **Use strdup() for mutable copies:**
   ```c
   var original:string = "Immutable";
   var mutable:string = strdup(original);
   mutable[0] = 'A';  // Safe
   free(mutable);
   ```

3. **Allocate enough space:**
   ```c
   var len1:int = strlen(str1);
   var len2:int = strlen(str2);
   var size:int = len1 + len2 + 1;  // +1 for null terminator
   var result:string = malloc(size);
   ```

4. **Always null-terminate manually built strings:**
   ```c
   var buffer:string = malloc(10);
   buffer[0] = 'H';
   buffer[1] = 'i';
   buffer[2] = '\0';  // Essential!
   ```

5. **Use strcmp() for content comparison:**
   ```c
   // Don't use: if (str1 == str2)  // Compares pointers
   // Do use:
   if (strcmp(str1, str2) == 0) {  // Compares content
       // ...
   }
   ```

---

## Summary

The compiler now provides full string support including:

- ✅ String variables and literals
- ✅ String indexing (read and write)
- ✅ String length (`strlen`)
- ✅ String comparison (`strcmp`)
- ✅ String copying (`strcpy`)
- ✅ String concatenation (`strcat`)
- ✅ String duplication (`strdup`)
- ✅ Heap allocation (`malloc`, `free`)
- ✅ Mutable strings via heap allocation
- ✅ Character-level string manipulation

This makes the language suitable for text processing, user input handling, and string-based algorithms.
