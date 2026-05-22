# DMM examples

Each `.dmm` file is a standalone program. From the compiler repository root,
compile and run an example with:

```sh
./build/compiler examples/calculator.dmm -o calculator
./calculator
```

On Windows, use `compiler.exe` and `-o calculator.exe`. Substitute your build
directory, such as `cmake-build-debug`, for `build`.

| Example | Language features demonstrated |
|---------|--------------------------------|
| [calculator.dmm](calculator.dmm) | `Result<int,string>` returns and exhaustive `match`, including division by zero |
| [guess_number.dmm](guess_number.dmm) | Tagged outcomes with integer payloads and arm-scoped match bindings |
| [shapes.dmm](shapes.dmm) | `Point<T>` specialization, by-value copies, an explicit `Equal` implementation with `Self`, and an inferred `T:Equal` function |
| [heap_management_demo.dmm](heap_management_demo.dmm) | Inferred generic allocation of `Cell<T>`, type-based `reserve`, and explicit `free` |
| [rock_paper_scissors.dmm](rock_paper_scissors.dmm) | Compile-time constants for named moves |
| [tictactoe.dmm](tictactoe.dmm) | Constant array lengths and fixed-array arguments passed as checked slices |
| [input_demo.dmm](input_demo.dmm) | Interactive integer and character input |
| [read_demo.dmm](read_demo.dmm) | Interactive formatted input |
| [read_function_guide.dmm](read_function_guide.dmm) | Formatted read usage and supported formats |
| [io_demo.dmm](io_demo.dmm) | File input/output and explicit buffer ownership |

The game and calculator demos use scripted inputs. The input/read demos prompt
for input. Heap allocations and concatenated strings remain explicitly owned;
copying a container does not release or duplicate an allocation.
