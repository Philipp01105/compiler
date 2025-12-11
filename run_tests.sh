#!/bin/bash

# =================================================================
# AUTOMATIC TEST RUNNER FOR DMM COMPILER
# =================================================================
# This script automatically:
# 1. Compiles .dmm test files to assembly (.s files)
# 2. Assembles them with gcc to create executables
# 3. Runs the executables and captures their output
# 4. Compares output against expected output files
# 5. Reports test results
# =================================================================

set -e  # Exit on error (but we'll handle test failures)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TEST_DIR="${SCRIPT_DIR}/tests"
OUTPUT_DIR="${SCRIPT_DIR}/test_output"
EXPECTED_DIR="${SCRIPT_DIR}/tests/expected"

# Compiler and tools
COMPILER="${BUILD_DIR}/compiler"
GCC="gcc"
GCC_FLAGS="-no-pie"

# Create necessary directories
mkdir -p "${OUTPUT_DIR}"
mkdir -p "${EXPECTED_DIR}"

# Print header
echo "================================================================="
echo "           DMM COMPILER - AUTOMATIC TEST RUNNER"
echo "================================================================="
echo ""
echo "Compiler: ${COMPILER}"
echo "Test Directory: ${TEST_DIR}"
echo "Output Directory: ${OUTPUT_DIR}"
echo ""

# Check if compiler exists
if [ ! -f "${COMPILER}" ]; then
    echo -e "${RED}ERROR: Compiler not found at ${COMPILER}${NC}"
    echo "Please build the compiler first with: cd build && cmake .. && make"
    exit 1
fi

# Function to compile a .dmm file
compile_dmm() {
    local dmm_file="$1"
    local base_name=$(basename "${dmm_file}" .dmm)
    local asm_file="${dmm_file}.s"
    local exe_file="${OUTPUT_DIR}/${base_name}"
    
    echo -n "  Compiling ${base_name}.dmm... "
    
    # Step 1: Compile to assembly
    if ! "${COMPILER}" "${dmm_file}" > /dev/null 2>&1; then
        echo -e "${RED}FAILED (compiler error)${NC}"
        return 1
    fi
    
    # Check if assembly file was created
    if [ ! -f "${asm_file}" ]; then
        echo -e "${RED}FAILED (no assembly generated)${NC}"
        return 1
    fi
    
    # Step 2: Assemble with gcc
    if ! ${GCC} ${GCC_FLAGS} "${asm_file}" -o "${exe_file}" 2>/dev/null; then
        echo -e "${RED}FAILED (assembly error)${NC}"
        return 1
    fi
    
    echo -e "${GREEN}OK${NC}"
    return 0
}

# Function to run a test and compare output
run_test() {
    local dmm_file="$1"
    local base_name=$(basename "${dmm_file}" .dmm)
    local exe_file="${OUTPUT_DIR}/${base_name}"
    local output_file="${OUTPUT_DIR}/${base_name}.out"
    local expected_file="${EXPECTED_DIR}/${base_name}.expected"
    
    ((TOTAL_TESTS++))
    
    echo ""
    echo -e "${BLUE}Test ${TOTAL_TESTS}: ${base_name}${NC}"
    echo "-------------------------------------------------------------------"
    
    # Compile the test
    if ! compile_dmm "${dmm_file}"; then
        ((FAILED_TESTS++))
        echo -e "${RED}✗ COMPILATION FAILED${NC}"
        return 1
    fi
    
    # Run the test and capture output
    echo -n "  Running test... "
    timeout 30s "${exe_file}" > "${output_file}" 2>&1
    local exit_code=$?
    
    # Check for timeout (exit code 124) or other catastrophic errors
    if [ ${exit_code} -eq 124 ]; then
        echo -e "${RED}TIMEOUT${NC}"
        ((FAILED_TESTS++))
        echo -e "${RED}✗ TEST FAILED${NC}"
        return 1
    elif [ ${exit_code} -gt 1 ]; then
        echo -e "${RED}CRASHED (exit code: ${exit_code})${NC}"
        ((FAILED_TESTS++))
        echo -e "${RED}✗ TEST FAILED${NC}"
        return 1
    fi
    echo -e "${GREEN}OK${NC}"
    
    # Compare output if expected file exists
    if [ -f "${expected_file}" ]; then
        echo -n "  Comparing output... "
        if diff -q "${output_file}" "${expected_file}" > /dev/null 2>&1; then
            echo -e "${GREEN}MATCH${NC}"
            ((PASSED_TESTS++))
            echo -e "${GREEN}✓ TEST PASSED${NC}"
            return 0
        else
            echo -e "${RED}MISMATCH${NC}"
            ((FAILED_TESTS++))
            echo -e "${RED}✗ TEST FAILED${NC}"
            echo ""
            echo "  Expected output in: ${expected_file}"
            echo "  Actual output in:   ${output_file}"
            echo ""
            echo "  Diff (first 20 lines):"
            diff "${expected_file}" "${output_file}" | head -20 || true
            return 1
        fi
    else
        echo -e "  ${YELLOW}No expected output file found${NC}"
        echo "  Output saved to: ${output_file}"
        echo ""
        echo "  To create expected output file, run:"
        echo "  cp ${output_file} ${expected_file}"
        ((PASSED_TESTS++))
        echo -e "${GREEN}✓ TEST PASSED (no comparison)${NC}"
        return 0
    fi
}

# Function to find all test files
find_tests() {
    # Look for .dmm files in tests directory
    if [ -d "${TEST_DIR}" ]; then
        find "${TEST_DIR}" -name "*.dmm" -type f | sort
    fi
}

# Main test execution
echo "================================================================="
echo "                     RUNNING TESTS"
echo "================================================================="
echo ""

# Find and run all tests
TEST_FILES=$(find_tests)

if [ -z "${TEST_FILES}" ]; then
    echo -e "${YELLOW}No test files found in ${TEST_DIR}${NC}"
    echo ""
    echo "To add tests:"
    echo "1. Create test files in: ${TEST_DIR}"
    echo "2. Create expected output files in: ${EXPECTED_DIR}"
    echo ""
    exit 0
fi

# Run each test
for test_file in ${TEST_FILES}; do
    run_test "${test_file}" || true  # Continue on failure
done

# Print summary
echo ""
echo "================================================================="
echo "                     TEST SUMMARY"
echo "================================================================="
echo ""
echo "Total Tests:  ${TOTAL_TESTS}"
echo -e "Passed:       ${GREEN}${PASSED_TESTS}${NC}"
echo -e "Failed:       ${RED}${FAILED_TESTS}${NC}"
echo ""

if [ ${FAILED_TESTS} -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    exit 1
fi
