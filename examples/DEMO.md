# Error Handling Fix Demo

## Issue
The compiler had two main problems:
1. Cascading errors - a single error would trigger multiple follow-up errors
2. Incorrect caret position - the error marker pointed to the wrong token

## Example

### Test Code
```dmm
func main() -> void {
    *xx = 20;
}
```

### Before (from issue description)
```
error[S100]: Variable 'xx' not found
 --> test.dmm:2:9
   |
2 |     *xx = 20;
   |         ^

error[P102]: Unexpected statement: expected expression or statement keyword
 --> test.dmm:2:9
  |
2 |     *xx = 20;
  |         ^^^^^^^^^

error[P102]: Unexpected statement: expected expression or statement keyword
 --> test.dmm:2:11
  |
2 |     *xx = 20;
  |           ^^^^^^^^^^^

error[P102]: Unexpected statement: expected expression or statement keyword
 --> test.dmm:2:13
  |
2 |     *xx = 20;
  |             ^^^^^^^^^^^^^

error: could not compile due to 4 errors
```

**Problems:**
- 4 errors reported for a single mistake
- Caret at column 9 (the `=` sign) instead of column 6 (the `xx` token)

### After (current implementation)
```
error[S100]: Variable 'xx' not found
 --> test.dmm:2:6
  |
2 |     *xx = 20;
  |      ^^

error: could not compile due to 1 error
```

**Improvements:**
- Only 1 error reported ✓
- Caret correctly at column 6, pointing to `xx` ✓
- Underline length matches token length (`^^` for 2-char token) ✓

## Multiple Errors Test

### Test Code
```dmm
func main() -> void {
    var x:int = 10;
    *undefinedPtr = 5;   // Error 1
    y = 20;              // Error 2
    z.field = 30;        // Error 3
    var w:int = 40;
    print(w);
}
```

### Result
```
error[S100]: Variable 'undefinedPtr' not found
 --> test_multiple_errors.dmm:3:6
  |
3 |     *undefinedPtr = 5;   // Error 1: undefinedPtr not found
  |      ^^^^^^^^^^^^

error[S100]: Variable 'y' not found
 --> test_multiple_errors.dmm:4:5
  |
4 |     y = 20;              // Error 2: y not found
  |     ^

error[S100]: Variable 'z' not found
 --> test_multiple_errors.dmm:5:5
  |
5 |     z.field = 30;        // Error 3: z not found
  |     ^

error: could not compile due to 3 errors
```

**Result:** Each error is reported once with correct positioning, and the parser continues to find all errors without cascading ✓
