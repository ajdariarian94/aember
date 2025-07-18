cmake_minimum_required(VERSION 3.19.5)

################################################################################
#
# Project definition
#
################################################################################
project(executables VERSION 1.0.0)

# Set language standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Executables
add_subdirectory(aember)
