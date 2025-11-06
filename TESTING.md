# Testing Guide for DMM Compiler

This document provides a comprehensive guide to the automatic testing system for the DMM compiler.

## Quick Start

Run all tests:
```bash
./run_tests.sh
```

## Overview

The automatic testing system eliminates manual testing by:
1. **Automatically discovering** all test files in `tests/` directory
2. **Compiling** each `.dmm` file to assembly
3. **Assembling** the code to an executable
4. **Running** each test program
5. **Comparing** output against expected results
6. **Reporting** clear pass/fail status

## Test Results

The test runner provides clear, colored output:

```
=================================================================
           DMM COMPILER - AUTOMATIC TEST RUNNER
=================================================================

Compiler: /path/to/build/compiler
Test Directory: /path/to/tests
Output Directory: /path/to/test_output

=================================================================
                     RUNNING TESTS
=================================================================

Test 1: arithmetic
-------------------------------------------------------------------
  Compiling arithmetic.dmm... OK
  Running test... OK
  Comparing output... MATCH
✓ TEST PASSED

Test 2: function
-------------------------------------------------------------------
  Compiling function.dmm... OK
  Running test... OK
  Comparing output... MATCH
✓ TEST PASSED

=================================================================
                     TEST SUMMARY
=================================================================

Total Tests:  9
Passed:       9
Failed:       0

✓ ALL TESTS PASSED
```

## Current Test Suite

The test suite includes 9 comprehensive tests:

| Test | Description | Features Tested |
|------|-------------|----------------|
| `hello.dmm` | Hello world | Basic I/O, println |
| `arithmetic.dmm` | Math operations | +, -, *, /, % operators |
| `loop.dmm` | For loop | Loop syntax, iteration |
| `function.dmm` | Function calls | Function definition, calling, return |
| `conditional.dmm` | If/else | Boolean conditions, branching |
| `else_if.dmm` | else if chains | Multiple condition testing |
| `nested_loops.dmm` | Nested loops | Loop nesting, variable scope |
| `string_ops.dmm` | String operations | String comparison, if with strings |
| `variables.dmm` | Variable handling | Declaration, assignment, types |

## Adding a New Test

### Step-by-Step Guide

#### 1. Create Test File

Create a new `.dmm` file in the `tests/` directory:

```bash
nano tests/my_feature.dmm
```

Example test content:

```javascript
// Test description: what this test verifies
func main() -> void {
    // Test code here
    println("Expected output");
}
```

#### 2. Run Test to Generate Output

```bash
./run_tests.sh
```

The test runner will:
- Compile and run your test
- Save the output to `test_output/my_feature.out`
- Tell you the test passed (without comparison)
- Show you the command to create the expected file

#### 3. Verify the Output

Check that the output is correct:

```bash
cat test_output/my_feature.out
```

If the output looks correct, proceed to step 4. If not, fix your test and repeat from step 2.

#### 4. Create Expected Output File

```bash
cp test_output/my_feature.out tests/expected/my_feature.expected
```

#### 5. Verify Test Passes

Run the tests again:

```bash
./run_tests.sh
```

Your test should now show:
```
Test X: my_feature
-------------------------------------------------------------------
  Compiling my_feature.dmm... OK
  Running test... OK
  Comparing output... MATCH
✓ TEST PASSED
```

### Complete Example

Let's add a test for the square function:

```bash
# 1. Create the test file
cat > tests/square.dmm << 'EOF'
// Square function test
func square(n:int) -> int {
    return n * n;
}

func main() -> void {
    println("Squares of 1-5:");
    for (var i:int = 1; i <= 5; i++) {
        var sq:int = square(i);
        println("  " + i + "^2 = " + sq);
    }
}
EOF

# 2. Run tests to generate output
./run_tests.sh

# 3. Check the output
cat test_output/square.out

# 4. Create expected file if output is correct
cp test_output/square.out tests/expected/square.expected

# 5. Verify test passes
./run_tests.sh
```

## Test Guidelines

### Do's

✓ **Keep tests simple and focused** - Each test should verify one feature  
✓ **Use descriptive names** - `string_concat.dmm` not `test3.dmm`  
✓ **Add comments** - Explain what the test verifies  
✓ **Test edge cases** - Include boundary conditions  
✓ **Make output deterministic** - Avoid timestamps, random values  
✓ **Keep tests fast** - Tests should complete in seconds  

### Don'ts

✗ **Don't test multiple unrelated features** - Split into separate tests  
✗ **Don't use random values** - Output must be reproducible  
✗ **Don't include timestamps** - Makes comparison fail  
✗ **Don't create infinite loops** - Tests timeout after 30 seconds  
✗ **Don't rely on external files** - Tests should be self-contained  

## Debugging Failed Tests

When a test fails, the runner shows:

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

### Debugging Steps

1. **View the actual output**:
   ```bash
   cat test_output/my_test.out
   ```

2. **View the expected output**:
   ```bash
   cat tests/expected/my_test.expected
   ```

3. **See full diff**:
   ```bash
   diff tests/expected/my_test.expected test_output/my_test.out
   ```

4. **Run test manually**:
   ```bash
   ./build/compiler tests/my_test.dmm
   gcc -no-pie tests/my_test.dmm.s -o /tmp/test
   /tmp/test
   ```

5. **Fix the issue**:
   - If the new output is correct: Update expected file
   - If the output is wrong: Fix the test or the compiler

## Test Categories

Organize tests by feature:

### Basic Tests
- Hello world
- Simple arithmetic
- Basic I/O

### Function Tests
- Function definitions
- Parameters
- Return values
- Recursion

### Control Flow Tests
- If/else statements
- else if chains
- For loops
- Nested loops
- Loop control (break, continue if implemented)

### Data Type Tests
- Integer operations
- Character handling
- String operations
- Boolean/bit operations
- Type conversions

### Advanced Tests
- Arrays
- Structs
- Methods
- Standard library functions
- Complex programs

### Regression Tests
Tests for previously fixed bugs to ensure they don't reoccur.

## Continuous Integration

The test runner is designed for CI/CD:

### Exit Codes
- `0` - All tests passed
- `1` - Some tests failed or error occurred

### GitHub Actions Example

```yaml
name: Test Compiler

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v2
    
    - name: Install Dependencies
      run: sudo apt-get update && sudo apt-get install -y cmake gcc
    
    - name: Build Compiler
      run: |
        mkdir build
        cd build
        cmake ..
        make
    
    - name: Run Tests
      run: ./run_tests.sh
```

## Performance Considerations

### Test Timeouts
- Default timeout: 30 seconds per test
- Adjust in `run_tests.sh` if needed
- Tests should complete in < 5 seconds

### Test Data Size
- Keep test programs small
- Large tests slow down the suite
- Split complex tests into smaller ones

### Parallel Execution
Current implementation runs tests sequentially. For parallel execution, consider using:
- GNU Parallel
- Make's `-j` flag
- Custom parallel runner

## Maintenance

### Regular Tasks

1. **Run tests before commits**:
   ```bash
   ./run_tests.sh
   ```

2. **Add tests for new features**:
   - Every new feature should have tests
   - Add tests before implementing features (TDD)

3. **Update expected outputs** when compiler behavior changes:
   ```bash
   # Review changes first!
   cp test_output/my_test.out tests/expected/my_test.expected
   ```

4. **Clean up test artifacts**:
   ```bash
   rm -rf test_output/
   ```

### When to Update Expected Files

Update expected output when:
- ✓ You intentionally changed compiler output
- ✓ You fixed a bug that changes output
- ✓ You improved output formatting

Don't update when:
- ✗ Test is failing due to a bug
- ✗ You haven't reviewed the changes
- ✗ Output contains errors or incorrect values

## Troubleshooting

### Problem: Compiler not found

```
ERROR: Compiler not found at /path/to/build/compiler
```

**Solution**: Build the compiler first:
```bash
mkdir -p build
cd build
cmake ..
make
cd ..
```

### Problem: Test times out

```
Test X: my_test
-------------------------------------------------------------------
  Compiling my_test.dmm... OK
  Running test... TIMEOUT
```

**Solution**: Check for:
- Infinite loops
- Input waiting (scanner)
- Heavy computation
- Increase timeout in script if needed

### Problem: Compilation fails

```
Test X: my_test
-------------------------------------------------------------------
  Compiling my_test.dmm... FAILED (compiler error)
```

**Solution**: Check syntax errors in test file:
```bash
./build/compiler tests/my_test.dmm
```

### Problem: Assembly fails

```
Test X: my_test
-------------------------------------------------------------------
  Compiling my_test.dmm... OK
  Running test... FAILED (assembly error)
```

**Solution**: Check generated assembly:
```bash
gcc -no-pie tests/my_test.dmm.s -o /tmp/test
```

### Problem: Tests pass locally but fail in CI

**Possible causes**:
- Non-deterministic output (random, timestamps)
- Different environment (locale, timezone)
- File system differences
- Uninitialized variables showing different values

**Solution**: Make tests deterministic and environment-independent.

## Advanced Usage

### Running Specific Tests

Currently not directly supported. Workaround:

```bash
# Test single file
./build/compiler tests/my_test.dmm
gcc -no-pie tests/my_test.dmm.s -o /tmp/test
/tmp/test
```

### Stress Testing

Create a stress test:

```javascript
// tests/stress.dmm
func main() -> void {
    for (var i:int = 0; i < 1000; i++) {
        // Intensive operations
    }
    println("Stress test complete");
}
```

### Memory Testing

Test with Valgrind (if available):

```bash
./build/compiler tests/my_test.dmm
gcc -no-pie -g tests/my_test.dmm.s -o /tmp/test
valgrind --leak-check=full /tmp/test
```

## Best Practices Summary

1. **Write tests first** (TDD approach when possible)
2. **Keep tests small** and focused
3. **Use descriptive names** for test files
4. **Document what each test verifies** in comments
5. **Run tests frequently** during development
6. **Don't skip failing tests** - fix them or remove them
7. **Update tests** when changing features
8. **Review diffs** before updating expected files
9. **Keep expected files in git** for history tracking
10. **Run full test suite** before pushing changes

## Resources

- Main README: [README.md](README.md)
- Test README: [tests/README.md](tests/README.md)
- Example programs: [code_examples/](code_examples/)
- Language docs: [examples/LANGUAGE_DOCUMENTATION.md](examples/LANGUAGE_DOCUMENTATION.md)

## Getting Help

If you have questions:
1. Check this document
2. Check [tests/README.md](tests/README.md)
3. Look at existing tests for examples
4. Open an issue on GitHub

## Contributing

When contributing tests:
1. Follow the guidelines in this document
2. Ensure all tests pass
3. Add documentation for complex tests
4. Update this guide if needed

---

**Last Updated**: November 2025  
**Version**: 1.0  
**Compiler Version**: 5.1.0
