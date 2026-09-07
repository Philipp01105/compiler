#!/usr/bin/env bash
# Additional native Linux validation without requiring CMake in WSL.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/test_output/linux"
mkdir -p "${OUT}"
sources=()
for source in "${ROOT}"/src/*.c; do
    [[ "${source}" == */syntax_converter_simple.c ]] || sources+=("${source}")
done
gcc -std=c2x "${sources[@]}" -o "${OUT}/compiler"
failed=0
for source in "${ROOT}"/tests/*.dmm; do
    name="$(basename "${source}" .dmm)"
    for syntax in intel att; do
        work="${OUT}/${name}_${syntax}"
        mkdir -p "${work}"
        cp "${source}" "${work}/input.dmm"
        if ! timeout 30 "${OUT}/compiler" "--syntax=${syntax}" "${work}/input.dmm" >"${work}/compile.log" 2>&1 ||
           ! gcc -no-pie "${work}/input.dmm.s" -o "${work}/program" >"${work}/link.log" 2>&1; then
            echo "FAIL compile/link ${name}/${syntax}: ${work}"
            failed=1; continue
        fi
        if (cd "${work}" && timeout 10 ./program >actual.out 2>runtime.err) &&
           diff -u <(sed 's/\r$//' "${ROOT}/tests/expected/${name}.expected") "${work}/actual.out" >"${work}/diff.log"; then
            echo "PASS ${name}/${syntax}"
        else
            echo "FAIL runtime/output ${name}/${syntax}: ${work}"
            failed=1
        fi
    done
done
for syntax in intel att; do
    work="${OUT}/core_abi_${syntax}"
    sed 's/main/dmm_test_entry/g' "${work}/input.dmm.s" >"${work}/interop.s"
    gcc -no-pie "${work}/interop.s" "${ROOT}/tests/abi_driver.c" -o "${work}/interop"
    if timeout 10 "${work}/interop"; then echo "PASS gcc-interop/${syntax}"; else failed=1; fi
done
exit "${failed}"
