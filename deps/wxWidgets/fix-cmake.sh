#!/bin/bash
find "$1" -name "CMakeLists.txt" -exec sed -i 's/cmake_minimum_required(VERSION [0-2]\.[0-9][0-9]*\(\.[0-9][0-9]*\)\?)/cmake_minimum_required(VERSION 3.5)/gI' {} \;
find "$1" -name "CMakeLists.txt" -exec sed -i 's/cmake_minimum_required(VERSION 3\.[0-4]\(\.[0-9][0-9]*\)\?)/cmake_minimum_required(VERSION 3.5)/gI' {} \;
