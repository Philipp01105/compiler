# Break and Continue Control Flow Statements

## Overview

The compiler now supports `break` and `continue` control flow statements for loop control, allowing for more flexible and expressive loop constructs.

## Features

### Break Statement

The `break` statement immediately terminates the innermost loop and transfers control to the statement immediately following the loop.

**Syntax:**
```javascript
break;
```

**Example:**
```javascript
func findFirst() -> void {
    for (var i:int = 0; i < 100; i++) {
        if (i == 42) {
            print("Found 42 at index " + i);
            break;  // Exit the loop immediately
        }
        print("Checking: " + i);
    }
    print("Loop terminated");
}
```

### Continue Statement

The `continue` statement skips the rest of the current iteration and proceeds directly to the next iteration of the loop.

**Syntax:**
```javascript
continue;
```

**Example:**
```javascript
func printOddNumbers() -> void {
    for (var i:int = 0; i < 10; i++) {
        if (i % 2 == 0) {
            continue;  // Skip even numbers
        }
        print("Odd: " + i);
    }
}
```

## Nested Loops

Both `break` and `continue` work correctly with nested loops. They affect only the innermost loop in which they appear.

**Example:**
```javascript
func nestedExample() -> void {
    for (var i:int = 0; i < 3; i++) {
        print("Outer: " + i);
        for (var j:int = 0; j < 5; j++) {
            if (j == 3) {
                break;  // Only breaks the inner loop
            }
            print("  Inner: " + j);
        }
    }
}
```

## Use Cases

### Early Termination

```javascript
func findSum() -> void {
    var sum:int = 0;
    for (var i:int = 1; i <= 100; i++) {
        var temp:int = sum + i;
        sum = temp;
        if (sum > 50) {
            print("Sum exceeded 50 at i=" + i);
            break;
        }
    }
}
```

### Filtering Elements

```javascript
func printMultiplesOfThree() -> void {
    for (var i:int = 0; i < 20; i++) {
        if (i % 3 != 0) {
            continue;  // Skip non-multiples
        }
        print(i);
    }
}
```

### Complex Conditions

```javascript
func processData() -> void {
    for (var i:int = 0; i < 10; i++) {
        if (i < 3) {
            continue;  // Skip first 3
        }
        if (i > 7) {
            break;  // Stop after 7
        }
        print("Processing: " + i);
    }
}
```

## Error Handling

The compiler validates that `break` and `continue` statements are only used within loops:

```javascript
func invalid() -> void {
    break;  // ERROR: 'break' statement not within a loop
}

func alsoInvalid() -> void {
    if (1 == 1) {
        continue;  // ERROR: 'continue' statement not within a loop
    }
}
```

## Implementation Details

### Assembly Generation

- **Break**: Generates a jump to the loop end label (`.L_for_end_<loop_id>`)
- **Continue**: Generates a jump to the loop increment label (`.L_for_increment_<loop_id>`)

### Loop Context Stack

The compiler maintains a loop context stack to track nested loops, ensuring that break and continue statements reference the correct loop labels.

### Example Assembly Output

For a break statement:
```assembly
    # Break statement (Line 8)
    jmp .L_for_end_0
```

For a continue statement:
```assembly
    # Continue statement (Line 20)
    jmp .L_for_increment_1
```

## Compatibility

- Works with `for` loops
- Compatible with nested loops (any depth)
- Can be used within `if`/`else` statements inside loops
- Full scope management maintained

## Testing

Comprehensive tests verify:
- Basic break functionality
- Basic continue functionality
- Nested loops with break
- Nested loops with continue
- Break within if statements
- Continue with complex conditions
- Error detection for break/continue outside loops

## Grammar Extension

```
statement ::= ...
            | 'break' ';'
            | 'continue' ';'
```

## Version

This feature is available starting with compiler version 4.6.0.
