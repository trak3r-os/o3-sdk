# SPDX-License-Identifier: GPL-3.0-or-later
# ctest script (cmake -P): runs bad_setup_sim (-DSIM=<exe> -DOUT=<scratch dir>) and checks that it fails loudly. A plain WILL_FAIL test
# would also pass on a crash or a missing file, so the exit status and the message are both checked.
file(REMOVE "${OUT}/badsetup.png")  # a leftover of an earlier run would be taken for this one's
execute_process(COMMAND "${SIM}" --ms 10 --out "${OUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 1)
  message(FATAL_ERROR "bad_setup_sim exited with '${status}', expected 1\nstdout: ${out}\nstderr: ${err}")
endif()
if(NOT err MATCHES "setup refused: set_source\\(slot 9, unit 0\\): slot, bus or index out of range")
  message(FATAL_ERROR "the refused call is not named on stderr:\n${err}")
endif()
if(NOT err MATCHES "1 refused call")
  message(FATAL_ERROR "the runner's own message is missing from stderr:\n${err}")
endif()
if(EXISTS "${OUT}/badsetup.png")
  message(FATAL_ERROR "bad_setup_sim wrote a picture although its setup failed")
endif()
