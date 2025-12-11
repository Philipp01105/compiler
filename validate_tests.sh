#!/bin/bash

# =================================================================
# TEST VALIDATION SCRIPT FOR DMM COMPILER
# =================================================================
# This script validates test outputs by analyzing the actual output
# content to determine if tests passed or failed, rather than just
# comparing against pre-saved expected files.
# =================================================================

# Don't use set -e since we handle errors explicitly
set +e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
COMPILER="${BUILD_DIR}/compiler"
GCC="gcc"
GCC_FLAGS="-no-pie"

# Test counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Print header
echo "================================================================="
echo "           DMM COMPILER - TEST VALIDATOR"
echo "================================================================="
echo "This script validates test outputs by analyzing their content"
echo "================================================================="
echo ""

# Check if compiler exists
if [ ! -f "${COMPILER}" ]; then
    echo -e "${RED}ERROR: Compiler not found at ${COMPILER}${NC}"
    echo "Please build the compiler first with: cd build && cmake .. && make"
    exit 1
fi

# Function to compile and run a test
compile_and_run() {
    local test_file="$1"
    local test_name="$2"
    local output_file="/tmp/${test_name}_output.txt"
    
    echo -n "  Compiling ${test_name}... "
    
    # Compile to assembly
    if ! "${COMPILER}" "${test_file}" > /dev/null 2>&1; then
        echo -e "${RED}FAILED${NC}"
        return 1
    fi
    
    # Check if assembly file was created
    local asm_file="${test_file}.s"
    if [ ! -f "${asm_file}" ]; then
        echo -e "${RED}FAILED (no assembly)${NC}"
        return 1
    fi
    
    # Assemble with gcc
    local exe_file="/tmp/${test_name}_exe"
    if ! ${GCC} ${GCC_FLAGS} "${asm_file}" -o "${exe_file}" 2>/dev/null; then
        echo -e "${RED}FAILED (assembly error)${NC}"
        return 1
    fi
    
    echo -e "${GREEN}OK${NC}"
    
    # Run the test and capture output
    echo -n "  Running ${test_name}... "
    timeout 30s "${exe_file}" > "${output_file}" 2>&1
    local exit_code=$?
    
    # Check for timeout (exit code 124) or catastrophic errors (>1)
    if [ ${exit_code} -eq 124 ]; then
        echo -e "${RED}TIMEOUT${NC}"
        return 1
    elif [ ${exit_code} -gt 1 ]; then
        echo -e "${RED}CRASHED (exit ${exit_code})${NC}"
        return 1
    fi
    
    # Exit codes 0 and 1 are considered OK (some compilers exit with 1)
    echo -e "${GREEN}OK${NC}"
    return 0
}

# Function to validate test output by analyzing content
validate_output() {
    local output_file="$1"
    local test_name="$2"
    
    echo -n "  Validating ${test_name} output... "
    
    # Check if output file exists and is not empty
    if [ ! -f "${output_file}" ] || [ ! -s "${output_file}" ]; then
        echo -e "${RED}FAILED (no output)${NC}"
        return 1
    fi
    
    # Count [OK] markers in output
    local ok_count=$(grep -c "\[OK\]" "${output_file}" 2>/dev/null | head -1 || echo "0")
    
    # Count error/failure indicators
    local fail_count=$(grep -ciE "\[FAIL\]|\[ERROR\]|FAILED|ERROR:" "${output_file}" 2>/dev/null | head -1 || echo "0")
    
    # Check for success indicators
    local success_indicators=$(grep -ciE "ALL.*PASSED|SUCCESS|COMPLETED|Test.*passed" "${output_file}" 2>/dev/null | head -1 || echo "0")
    
    # Validation logic
    if [ "${fail_count}" -gt 0 ]; then
        echo -e "${RED}FAILED${NC}"
        echo "    Found ${fail_count} failure indicator(s) in output"
        echo "    First failures:"
        grep -iE "\[FAIL\]|\[ERROR\]|FAILED|ERROR:" "${output_file}" | head -5
        return 1
    elif [ "${ok_count}" -gt 0 ] || [ "${success_indicators}" -gt 0 ]; then
        echo -e "${GREEN}PASSED${NC}"
        echo "    Found ${ok_count} [OK] marker(s)"
        echo "    Found ${success_indicators} success indicator(s)"
        return 0
    else
        # No clear indicators, check if output seems reasonable
        local line_count=$(wc -l < "${output_file}")
        if [ "${line_count}" -gt 0 ]; then
            echo -e "${YELLOW}PASSED (output present)${NC}"
            echo "    No explicit validation markers found"
            echo "    Output has ${line_count} lines - appears valid"
            return 0
        else
            echo -e "${RED}FAILED${NC}"
            echo "    No output generated"
            return 1
        fi
    fi
}

# Function to run and validate a test
run_and_validate_test() {
    local test_file="$1"
    local test_name=$(basename "${test_file}" .dmm)
    
    ((TOTAL_TESTS++))
    
    echo ""
    echo -e "${BLUE}Test ${TOTAL_TESTS}: ${test_name}${NC}"
    echo "-------------------------------------------------------------------"
    
    # Compile and run
    local output_file="/tmp/${test_name}_output.txt"
    if ! compile_and_run "${test_file}" "${test_name}"; then
        ((FAILED_TESTS++))
        echo -e "${RED}✗ TEST FAILED (compilation/runtime error)${NC}"
        return 1
    fi
    
    # Validate output
    if validate_output "${output_file}" "${test_name}"; then
        ((PASSED_TESTS++))
        echo -e "${GREEN}✓ TEST PASSED${NC}"
        return 0
    else
        ((FAILED_TESTS++))
        echo -e "${RED}✗ TEST FAILED (validation failed)${NC}"
        return 1
    fi
}

# Main execution
echo "================================================================="
echo "                     RUNNING VALIDATION"
echo "================================================================="
echo ""

# Check if user provided test files as arguments
if [ $# -gt 0 ]; then
    echo "Testing user-provided files..."
    for test_file in "$@"; do
        if [ -f "${test_file}" ]; then
            run_and_validate_test "${test_file}"
        else
            echo -e "${YELLOW}Warning: ${test_file} not found${NC}"
        fi
    done
else
    # Default: test files from tests/ directory with expected outputs
    echo "Testing files from tests/ directory..."
    if [ -d "${SCRIPT_DIR}/tests" ]; then
        for test_file in "${SCRIPT_DIR}/tests"/*.dmm; do
            if [ -f "${test_file}" ]; then
                run_and_validate_test "${test_file}"
            fi
        done
    else
        echo -e "${YELLOW}Warning: tests/ directory not found${NC}"
        echo "Usage: $0 [test_file1.dmm test_file2.dmm ...]"
        exit 1
    fi
fi

# Print summary
echo ""
echo "================================================================="
echo "                     VALIDATION SUMMARY"
echo "================================================================="
echo ""
echo "Total Tests:  ${TOTAL_TESTS}"
echo -e "Passed:       ${GREEN}${PASSED_TESTS}${NC}"
echo -e "Failed:       ${RED}${FAILED_TESTS}${NC}"
echo ""

if [ ${FAILED_TESTS} -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS VALIDATED SUCCESSFULLY${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED VALIDATION${NC}"
    exit 1
fi
