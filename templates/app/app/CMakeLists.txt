# SPDX-License-Identifier: GPL-3.0-or-later
# @APP_NAME@: the app. o3sdk_app() (o3-sdk/cmake/O3sdkTargets.cmake) builds the library `app_@APP_ID@`, the simulator `@APP_ID@_sim` on the
# host and, with -DO3SDK_PLATFORM=mk66, the image `@APP_ID@.elf` / `.hex` / `.bin` / `.ptf` with its ctests (`@APP_ID@_image`,
# `@APP_ID@_flashconfig`, `@APP_ID@_ramfunc`, `@APP_ID@_usb`).
o3sdk_app(@APP_ID@ SOURCES src/app.cpp src/play_screen.cpp DEPENDS unit_voice
  # The boot logo of this image: the kit's O3 logo unless you name a PNG (a path from this directory), which the build converts and links
  # instead (docs/NEW-APP.md, "The boot logo"). A grey picture is drawn white, or in LOGO_COLOR; a picture with colour in its own colours.
  # LOGO ../logo/logo.png
  # LOGO_COLOR 0xFF8000
)
# <@APP_ID@/pad_map.h>: the pure parts of the app, which the tests include.
target_include_directories(app_@APP_ID@ PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
