# Doudou firmware — ESP-IDF top-level CMake.
#
# Build:
#   cd packages/firmware
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32c3
#   idf.py build
#   idf.py flash monitor
#
# Target chip: ESP32-C3 (ESP32-2424S012 board)

cmake_minimum_required(VERSION 3.16)

# Use the project-local partitions.csv (also referenced from docs/firmware/).
set(PARTITION_TABLE_CSV_PATH "${CMAKE_CURRENT_LIST_DIR}/partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doudou)
