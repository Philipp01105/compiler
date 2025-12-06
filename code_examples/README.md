# Code Examples and Game Demonstrations

This directory contains example programs and games that demonstrate the features and capabilities of the compiler. Each example is written in the custom language and showcases different aspects of the language syntax and standard library.

## Overview

These examples serve as:
- **Tests** - Validation that the compiler works correctly with real-world programs
- **Demonstrations** - Showcasing language features and capabilities
- **Learning Resources** - Examples for new users to understand the language
- **Benchmarks** - Complex programs to test compiler performance

## Games and Examples

### 1. Shapes - Nested Structs (`shapes.txt`)

A demonstration of nested struct usage with geometric shapes.

**Features Demonstrated:**
- Nested struct composition (Rectangle contains two Point structs)
- Accessing nested struct fields
- Struct field manipulation
- Geometric calculations
- Multiple demonstrations of the same concept

**Key Language Features:**
- Nested structs (struct fields of another struct type)
- Direct field access on nested structs (e.g., `rect.topLeft.x`)
- Struct instances without methods (current limitation)
- Calculations using nested struct fields
- State manipulation through field updates

**How to Run:**
```bash
./build/compiler code_examples/shapes.txt
gcc -no-pie code_examples/shapes.txt.s -o shapes
./shapes
```

**Output:** Shows various rectangle and square demonstrations including point manipulation and resizing.

**Note:** This example demonstrates the current state of nested struct support. Methods cannot yet access nested struct fields, so all calculations are done in external functions.

---

### 2. TicTacToe (`tictactoe.txt`)

A complete Tic Tac Toe game implementation using structs.

**Features Demonstrated:**
- Struct usage with methods
- Game board representation using individual fields
- Win condition checking (rows, columns, diagonals)
- AI opponent with simple strategy
- Turn-based gameplay
- State management

**Key Language Features:**
- Struct definition with multiple fields
- Methods accessing struct fields directly
- Control flow (if-else chains, for loops)
- Boolean logic for win detection
- Method calls on struct instances

**How to Run:**
```bash
./build/compiler code_examples/tictactoe.txt
gcc -no-pie code_examples/tictactoe.txt.s -o tictactoe
./tictactoe
```

**Output:** Shows two game simulations - one with predetermined moves and one with AI vs AI.

---

### 3. Number Guessing Game (`guess_number.txt`)

A number guessing game where players try to find a secret number within a limited number of guesses.

**Features Demonstrated:**
- Struct-based game state management
- Range narrowing logic
- Guess validation and feedback
- Binary search strategy demonstration
- Array usage for storing guesses

**Key Language Features:**
- Struct methods with parameters and return values
- Conditional logic (if-else if chains)
- Array manipulation
- Game state tracking
- Mathematical operations (division for binary search)

**How to Run:**
```bash
./build/compiler code_examples/guess_number.txt
gcc -no-pie code_examples/guess_number.txt.s -o guess_number
./guess_number
```

**Output:** Two simulations - manual guessing and optimized binary search strategy.

---

### 4. Rock Paper Scissors (`rock_paper_scissors.txt`)

Classic Rock-Paper-Scissors game implementation.

**Features Demonstrated:**
- Multi-round tournament system
- Score tracking across rounds
- Game outcome determination
- Enum-like constants (0=Rock, 1=Paper, 2=Scissors)
- Pattern-based strategies

**Key Language Features:**
- Struct methods for game logic
- Complex conditional logic for determining winners
- Score accumulation
- Array usage for move sequences
- String handling for move names
- Helper functions outside structs

**How to Run:**
```bash
./build/compiler code_examples/rock_paper_scissors.txt
gcc -no-pie code_examples/rock_paper_scissors.txt.s -o rps
./rps
```

**Output:** Best-of-5 game and a 10-round tournament with different strategies.

---

### 5. Calculator (`calculator.txt`)

A feature-rich calculator with memory functions and operation history.

**Features Demonstrated:**
- Multiple mathematical operations
- Memory storage and recall
- Operation chaining
- State persistence
- Absolute value computation

**Key Language Features:**
- Struct with many methods
- Method composition (performing operations in sequence)
- Memory management (store, recall, add to memory)
- Conditional operations (division by zero check)
- Mathematical operations (+, -, *, /, %, square, abs)

**How to Run:**
```bash
./build/compiler code_examples/calculator.txt
gcc -no-pie code_examples/calculator.txt.s -o calculator
./calculator
```

**Output:** Multiple demonstrations of calculator features including basic ops, memory functions, and chained calculations.

---

## Compilation Instructions

### Compile All Examples

```bash
cd /home/runner/work/compiler/compiler
for game in code_examples/*.txt; do
    echo "Compiling $(basename $game)..."
    ./build/compiler "$game"
    gcc -no-pie "$game.s" -o "$(basename $game .txt)"
done
```

### Compile Individual Example

```bash
./build/compiler code_examples/tictactoe.txt
gcc -no-pie code_examples/tictactoe.txt.s -o tictactoe_game
./tictactoe_game
```

## Language Features Showcased

### Structs
All examples use structs extensively to organize game state and methods:
```javascript
struct GameName {
    int field1;
    int field2;
    
    func methodName() -> void {
        // Access fields directly
        field1 = field1 + 1;
    }
}
```

### Arrays
Used for storing game moves, board states, and sequences:
```javascript
var[10] moves:int;
moves[0] = 5;
```

### Control Flow
- **if-else if chains**: Complex decision making
- **for loops**: Iterating through moves and rounds
- **while logic**: Game loop conditions

### Functions and Methods
- Methods with parameters and return values
- Helper functions for utility operations
- Method calls on struct instances

### Standard Library Usage
All examples import and use functions from:
- `stdlib/core.txt` - Basic operations
- `stdlib/utils.txt` - Utility functions (printLine, printLabeled, etc.)
- `stdlib/math.txt` - Mathematical operations

## Known Limitations

### Struct Method Limitations
Currently, methods within a struct **cannot call other methods** from the same struct. This is a known limitation documented in `ISSUE_STRUCT_METHOD_CALLS.md`.

**Workaround:** Inline the logic or use external helper functions.

Example:
```javascript
// This does NOT work:
struct Game {
    func helper() -> int { return 5; }
    
    func main() -> void {
        var x:int = helper();  // ERROR: Function 'helper' not found
    }
}

// Workaround - inline the logic:
struct Game {
    func main() -> void {
        var x:int = 5;  // Inline the helper logic
    }
}
```

### String Concatenation in Struct Methods
Complex string concatenation with multiple struct fields in a single print statement may cause issues. 

**Workaround:** Use `printLabeled()` from stdlib/utils.txt or separate print statements.

```javascript
// Instead of:
print("Score: " + field1 + " | Round: " + field2);

// Use:
printLabeled("Score", field1);
printLabeled("Round", field2);
```

## Adding New Examples

When creating new example programs:

1. **Use structs** to organize state and behavior
2. **Import standard library** modules as needed
3. **Add documentation** at the top explaining what the example demonstrates
4. **Test compilation** before committing
5. **Update this README** with the new example

### Template

```javascript
// =================================================================
// EXAMPLE NAME
// Brief description
// =================================================================
// This example demonstrates:
// - Feature 1
// - Feature 2
// - Feature 3
// =================================================================

#import <stdlib/core.txt>
#import <stdlib/utils.txt>

struct ExampleStruct {
    int field1;
    
    func init() -> void {
        field1 = 0;
    }
    
    func doSomething() -> void {
        // Implementation
    }
}

func main() -> void {
    var example:ExampleStruct;
    example.init();
    example.doSomething();
}
```

## Testing

To verify all examples compile correctly:

```bash
cd /home/runner/work/compiler/compiler
./test_examples.sh  # If test script exists
# Or manually:
for game in code_examples/*.txt; do
    ./build/compiler "$game" || echo "FAILED: $game"
done
```

## Performance Notes

- **TicTacToe**: ~126 KB assembly, demonstrates struct methods
- **Calculator**: Most complex struct with ~20 methods
- **Guess Number**: Demonstrates algorithm optimization (binary search)
- **Rock Paper Scissors**: Simple game logic with multiple scenarios

## Future Examples

Potential examples to add:
- **Hangman** - String manipulation and character guessing
- **Maze Solver** - Path finding algorithms
- **Card Game** - Deck shuffling and card dealing
- **Quiz Game** - Question/answer system
- **Math Puzzles** - Sudoku, equations, etc.

## Contributing

When adding new examples:
1. Follow the existing code style
2. Add comprehensive comments
3. Test thoroughly
4. Document any language features used
5. Update this README

## License

These examples are part of the compiler project and follow the same license as the main repository.

## Version

Examples Version: 1.0.0  
Compatible with Compiler Version: 4.0.1+  
Date: November 2025

---

**Note:** All examples are fully functional and tested. They demonstrate real-world usage of the language and serve as both tests and learning resources.
