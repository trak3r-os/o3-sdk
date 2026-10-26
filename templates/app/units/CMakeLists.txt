# SPDX-License-Identifier: GPL-3.0-or-later
# This app's own units (DSP only, against o3-sdk's dsp layer): one directory each with its CMakeLists.txt, which calls o3sdk_unit().
# A directory with a CMakeLists.txt is picked up here; the app lists the unit in its DEPENDS (app/CMakeLists.txt).
file(GLOB _unit_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(_dir IN LISTS _unit_dirs)
  if(EXISTS "${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()
