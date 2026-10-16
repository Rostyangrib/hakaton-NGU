#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p build
compiler="${CXX:-g++}"
"$compiler" --version
"$compiler" -std=c++20 -O2 -Wall -Wextra -Wpedantic solution.cpp -o build/solution-cpp
if [[ "${1:-}" == "--test" ]]; then
    "$compiler" -std=c++20 -O2 -Wall -Wextra -Wpedantic solution_cpp_test.cpp -o build/solution-cpp-tests
    ./build/solution-cpp-tests | tee build/unit-tests-cpp-linux.log
fi
