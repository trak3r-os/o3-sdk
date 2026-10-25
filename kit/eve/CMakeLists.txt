# SPDX-License-Identifier: GPL-3.0-or-later
# eve: FT81x display-list encoders and the frame builder. Depends on hal (and through it core).
o3sdk_library(eve SOURCES src/display_list_builder.cpp DEPENDS hal)
