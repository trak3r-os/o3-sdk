# SPDX-License-Identifier: GPL-3.0-or-later
#
# Toolchain file of the mk66 and qemu builds: the arm-none-eabi toolchain (cmake/arm-none-eabi-toolchain.cmake, which says how
# the compiler is found: -DARM_GNU_ROOT=<dir>, the environment, or PATH). Kept as its own name because the build files and
# the documents refer to it.
include("${CMAKE_CURRENT_LIST_DIR}/arm-none-eabi-toolchain.cmake")
