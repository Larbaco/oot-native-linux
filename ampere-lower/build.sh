#!/bin/sh
# Build ampere-lower with the system C++ compiler (Xcode command line tools on macOS, any clang/gcc with
# C++20 elsewhere). Output: ./ampere-lower next to this script.
set -eu
cd "$(dirname "$0")"
CXX=${CXX:-c++}
"$CXX" -std=c++20 -O2 -Wall -Wno-unused-function -I. main.cpp -o ampere-lower
echo "built $(pwd)/ampere-lower"
