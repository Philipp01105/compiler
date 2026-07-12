# Runtime performance benchmarks

The opt-in benchmark suite compares generated programs compiled at `-O0` and `-O1`. It checks program output, performs
two warmup runs and records the median of seven timed runs per variant. Results and individual samples are written to
`performance/report.json` under the selected build directory.

```sh
cmake -S . -B build -DDMM_ENABLE_PERF_TESTS=ON
cmake --build build --target compiler
ctest --test-dir build -R '^runtime_performance$' --output-on-failure
```

Run the harness directly to change the sample counts:

```sh
python tests/performance/run.py \
  --compiler build/compiler \
  --output-dir build/performance \
  --warmups 2 \
  --samples 7
```

On Windows, pass `build/compiler.exe` or the executable path used by the selected CMake generator.

To compare a build with a report captured on the same machine, copy the earlier report outside the output directory and
pass it as a baseline:

```sh
python tests/performance/run.py \
  --compiler build/compiler \
  --output-dir build/performance \
  --baseline previous-report.json \
  --max-regression-percent 10
```

The command fails when the measured `-O1` median exceeds the configured regression threshold relative to the baseline.
The CTest benchmark intentionally has no default threshold because scheduler load and power management can dominate
short measurements. Compare results only on the same idle machine and with the same compiler, target and build
configuration.
