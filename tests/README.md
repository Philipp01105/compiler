# Automatic Testing System for DMM Compiler

This directory contains the automatic testing infrastructure for the DMM compiler.

## Overview

The testing system automatically:
1. Compiles `.dmm` test files to assembly (`.s` files)
2. Assembles them with `gcc` to create executables
3. Runs the executables and captures their output
4. Compares output against expected output files
5. Reports test results with clear pass/fail indicators

## Directory Structure

```
tests/
├── README.md                 # This file
├── expected/                 # Expected output files
│   ├── hello.expected
│   ├── arithmetic.expected
│   └── ...
├── hello.dmm                 # Test source files
├── arithmetic.dmm
└── ...
```

## Running Tests

### Run All Tests

```bash
./run_tests.sh
```

This will:
- Find all `.dmm` files in the `tests/` directory
- Compile and run each test
- Compare output against expected results
- Display a summary of passed/failed tests

### Run from Build Directory

If you're in the `build/` directory:

```bash
cd ..
./run_tests.sh
```

## Adding New Tests

### Step 1: Create a Test File

Create a new `.dmm` file in the `tests/` directory:

```bash
nano tests/my_test.dmm
```

Example test content:

```javascript
// My test description
func main() -> void {
    println("Test output");
}
```

### Step 2: Generate Expected Output

Run the test runner once to generate the actual output:

```bash
./run_tests.sh
```

The script will tell you that no expected output file was found and show you the command to create it:

```bash
cp test_output/my_test.out tests/expected/my_test.expected
```

### Step 3: Verify the Expected Output

Check that the output is correct:

```bash
cat tests/expected/my_test.expected
```

If it looks correct, the test is ready! If not, fix your test file and repeat from Step 2.

### Step 4: Run Tests Again

Now when you run the tests, your new test will be compared against the expected output:

```bash
./run_tests.sh
```

## Test Examples

### Basic Hello World

**File:** `tests/hello.dmm`
```javascript
func main() -> void {
    println("Hello, World!");
}
```

**Expected:** `tests/expected/hello.expected`
```
Hello, World!
```

### Arithmetic Operations

**File:** `tests/arithmetic.dmm`
```javascript
func main() -> void {
    var a:int = 10;
    var b:int = 5;
    
    println("Addition: " + (a + b));
    println("Subtraction: " + (a - b));
    println("Multiplication: " + (a * b));
    println("Division: " + (a / b));
    println("Modulo: " + (a % b));
}
```

### Loop Test

**File:** `tests/loop.dmm`
```javascript
func main() -> void {
    println("Counting from 1 to 5:");
    for (var i:int = 1; i <= 5; i++) {
        println("  " + i);
    }
    println("Done!");
}
```

### Function Call Test

**File:** `tests/function.dmm`
```javascript
func add(x:int, y:int) -> int {
    return x + y;
}

func main() -> void {
    var result:int = add(7, 3);
    println("7 + 3 = " + result);
}
```

## Test Output

The test runner produces colored output:

- 🟢 **GREEN** - Test passed
- 🔴 **RED** - Test failed
- 🟡 **YELLOW** - Warning or info

Example output:

```
=================================================================
           DMM COMPILER - AUTOMATIC TEST RUNNER
=================================================================

Compiler: /path/to/compiler
Test Directory: /path/to/tests
Output Directory: /path/to/test_output

=================================================================
                     RUNNING TESTS
=================================================================

Test 1: hello
-------------------------------------------------------------------
  Compiling hello.dmm... OK
  Running test... OK
  Comparing output... MATCH
✓ TEST PASSED

=================================================================
                     TEST SUMMARY
=================================================================

Total Tests:  4
Passed:       4
Failed:       0

✓ ALL TESTS PASSED
```

## Debugging Failed Tests

When a test fails, the runner will show you:

1. Which compilation or execution step failed
2. The location of the expected and actual output files
3. A diff showing the differences (first 20 lines)

Example:

```
Test 3: my_test
-------------------------------------------------------------------
  Compiling my_test.dmm... OK
  Running test... OK
  Comparing output... MISMATCH
✗ TEST FAILED

  Expected output in: tests/expected/my_test.expected
  Actual output in:   test_output/my_test.out

  Diff (first 20 lines):
  1c1
  < Expected line
  ---
  > Actual line
```

To debug:

```bash
# View actual output
cat test_output/my_test.out

# View expected output
cat tests/expected/my_test.expected

# View full diff
diff tests/expected/my_test.expected test_output/my_test.out

# Run the test manually
./test_output/my_test
```

## Best Practices

1. **Keep tests simple and focused** - Each test should verify one feature or behavior
2. **Use descriptive filenames** - `arithmetic.dmm` is better than `test1.dmm`
3. **Add comments** - Explain what each test is checking
4. **Test edge cases** - Include tests for boundary conditions and error cases
5. **Keep expected output stable** - Avoid timestamps or random values in output

## Test Categories

Consider organizing tests into categories:

- **Basic tests** - Hello world, simple arithmetic, basic I/O
- **Function tests** - Function calls, return values, parameters
- **Control flow tests** - If/else, loops, nested structures
- **Data type tests** - int, char, string, arrays, structs
- **Standard library tests** - Testing stdlib functions
- **Integration tests** - Complex programs using multiple features
- **Regression tests** - Tests for previously fixed bugs

## Continuous Integration

The test runner returns appropriate exit codes:
- `0` - All tests passed
- `1` - Some tests failed or error occurred

This makes it easy to integrate with CI/CD systems:

```yaml
# Example GitHub Actions workflow
- name: Run Compiler Tests
  run: ./run_tests.sh
```

## Cleaning Up

To clean up test artifacts:

```bash
# Remove compiled outputs
rm -rf test_output/

# Remove generated assembly files
rm -f tests/*.s
```

## Troubleshooting

### Compiler not found

**Error:** `ERROR: Compiler not found at /path/to/build/compiler`

**Solution:** Build the compiler first:
```bash
mkdir -p build
cd build
cmake ..
make
cd ..
```

### Tests timeout

**Error:** `TIMEOUT`

**Solution:** The test is taking too long (>30 seconds). Check for:
- Infinite loops
- Waiting for input that never comes
- Heavy computation

### Tests crash

**Error:** `CRASHED (exit code: X)`

**Solution:** The program crashed. Check for:
- Segmentation faults
- Division by zero
- Array out of bounds
- Null pointer dereference

## Contributing

When adding new features to the compiler, please:

1. Add corresponding tests
2. Ensure all existing tests still pass
3. Update this README if adding new test categories

## License

Tests are part of the compiler project and follow the same license.
