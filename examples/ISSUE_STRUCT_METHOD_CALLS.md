# Issue: Methods in Structs Cannot Call Other Methods of the Same Struct

## Summary

Methods defined within a struct cannot call other methods from the same struct. This significantly limits the usefulness of struct methods and forces code duplication or awkward workarounds.

## Current Behavior

When a method tries to call another method from the same struct, the compiler reports:
```
Error: Function 'methodName' not found
```

## Example Code

```c
struct TicTacToe {
    int board0;
    int board1;
    int board2;
    
    func getBoardValue(pos:int) -> int {
        if (pos == 0) {
            return board0;
        } else if (pos == 1) {
            return board1;
        }
        return board2;
    }
    
    func printCell(pos:int) -> void {
        var value:int = getBoardValue(pos);  // ERROR: Function 'getBoardValue' not found
        if (value == 0) {
            print(pos);
        } else {
            print("X");
        }
    }
}
```

## Expected Behavior

Methods should be able to call other methods from the same struct, similar to how it works in C++, Java, and other OOP languages:

```c
struct Point {
    int x;
    int y;
    
    func getX() -> int {
        return x;
    }
    
    func getY() -> int {
        return y;
    }
    
    func distanceFromOrigin() -> int {
        // Should be able to call getX() and getY()
        var dx:int = getX();
        var dy:int = getY();
        return dx * dx + dy * dy;
    }
}
```

## Current Workarounds

### Workaround 1: Inline all code (causes duplication)
```c
struct TicTacToe {
    int board0;
    int board1;
    
    func printCell(pos:int) -> void {
        // Duplicate the getBoardValue logic inline
        var value:int = 0;
        if (pos == 0) {
            value = board0;
        } else if (pos == 1) {
            value = board1;
        }
        
        if (value == 0) {
            print(pos);
        } else {
            print("X");
        }
    }
}
```

### Workaround 2: Use external helper functions (loses encapsulation)
```c
struct TicTacToe {
    int board0;
    int board1;
}

// Helper functions outside struct
func getBoardValue(game:TicTacToe, pos:int) -> int {
    if (pos == 0) {
        return game.board0;
    }
    return game.board1;
}
```

## Impact

This limitation:
1. **Reduces code reusability**: Cannot break down complex methods into smaller helper methods
2. **Increases code duplication**: Same logic must be repeated across multiple methods
3. **Makes code harder to maintain**: Changes must be replicated in multiple places
4. **Limits OOP design patterns**: Cannot implement common patterns that rely on method composition
5. **Creates inconsistency**: Fields can be accessed directly, but methods cannot

## Suggested Implementation

The method resolution should:
1. Check if a function call is being made within a struct method
2. First look for methods in the current struct context
3. Generate the appropriate mangled method name (e.g., `StructName_methodName`)
4. Pass the implicit `this` pointer as the first argument

## Technical Details

From the struct documentation, methods are compiled with name mangling:
- Original: `Point.move()`
- Mangled: `Point_move`

When a method calls another method of the same struct, it should:
1. Detect it's in a struct method context (via `Parser.current_struct_context`)
2. Look up the method in the struct's method list
3. Generate a call with the mangled name
4. Pass the same `this` pointer that the current method received

## Related Files

- `src/parser.c`: Method call parsing and struct context tracking
- `src/parser.h`: `Parser.current_struct_context` field
- `examples/STRUCT_FEATURE_DOCUMENTATION.md`: Struct feature documentation

## Priority

**Medium-High** - This is a significant limitation that affects the usability of the struct feature and forces unnatural code patterns.

## Additional Context

This issue was discovered while implementing example games in `code_examples/tictactoe.txt`, where natural OOP design patterns (helper methods, method decomposition) could not be used due to this limitation.

## Example Use Case

A realistic TicTacToe game implementation would benefit from:
```c
struct TicTacToe {
    int board0;
    int board1;
    // ... more board fields
    
    func getBoardValue(pos:int) -> int { /* ... */ }
    func setBoardValue(pos:int, value:int) -> void { /* ... */ }
    
    func checkLine(a:int, b:int, c:int) -> bit {
        // Should call getBoardValue()
        var valA:int = getBoardValue(a);
        var valB:int = getBoardValue(b);
        var valC:int = getBoardValue(c);
        // ... comparison logic
    }
    
    func checkWinner() -> int {
        // Should call checkLine()
        if (checkLine(0, 1, 2) == 1) {
            return getBoardValue(0);
        }
        // ... more checks
    }
}
```

Without method-to-method calls, the code becomes significantly more verbose and harder to maintain.
