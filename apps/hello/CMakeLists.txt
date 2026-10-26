# SPDX-License-Identifier: GPL-3.0-or-later
# HELLO: an example whole app (a synth, a delay, a riff; docs/FRAMEWORK.md, "Apps"). A new app starts from templates/app, in its own repository
# (tools/new_app.py, docs/NEW-APP.md).
o3sdk_app(hello SOURCES src/hello_app.cpp DEPENDS unit_synth unit_delay)
