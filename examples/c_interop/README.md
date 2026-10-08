# C sensor-analysis interop

DMM owns a sample array and calls a small C analysis function. C calls the DMM
`sensor_calibrate` export for each sample, converting Celsius to Fahrenheit,
then writes a native `SensorStats` structure back to DMM. This exercises both
directions of the platform C ABI.

Linux, from the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -c examples/c_interop/sensor.c -o build/sensor.o
./build/compiler --native-library sensor=build/sensor.o examples/c_interop/c_interop.dmm -o build/c_interop
./build/c_interop
```

Windows with MinGW-w64:

```powershell
gcc -std=c11 -Wall -Wextra -Werror -c examples/c_interop/sensor.c -o cmake-build-debug/sensor.o
.\cmake-build-debug\compiler.exe --native-library sensor=cmake-build-debug/sensor.o examples/c_interop/c_interop.dmm -o cmake-build-debug/c-interop.exe
.\cmake-build-debug\c-interop.exe
```

The three samples produce minimum 68, maximum 75.2 and mean 71.6 Fahrenheit.
The header defines the C layout; the DMM `extern "system"` declaration uses native
field widths and calling conventions. Pointer casts are confined to the FFI
call. C borrows the buffers only for the duration of that call and does not retain,
allocate or free them. Compile the C object for the same target as the DMM
executable. `--native-library` resolves the logical `sensor` import to that object.
