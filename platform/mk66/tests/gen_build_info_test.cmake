# SPDX-License-Identifier: GPL-3.0-or-later
# Test of tools/gen_build_info.cmake (ctest `mk66_build_info`): the two identities (the kit's and the app's), the "unknown" outside git,
# "-dirty", the version tag, one tree for both when the app is o3sdk's own, and a header that is only written when its content changes.
# It builds throw-away git repositories under WORK; GIT_CEILING_DIRECTORIES keeps git from finding the o3sdk repository above them.
#   cmake -DGEN=<gen_build_info.cmake> -DWORK=<scratch dir> -P gen_build_info_test.cmake
if(NOT GEN OR NOT WORK)
  message(FATAL_ERROR "usage: cmake -DGEN=<gen_build_info.cmake> -DWORK=<scratch dir> -P gen_build_info_test.cmake")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
get_filename_component(WORK "${WORK}" REALPATH)

# The environment the git calls (here and in the script under test) run in: no user configuration, a fixed identity and dates.
set(ENV{GIT_CEILING_DIRECTORIES} "${WORK}")
set(ENV{GIT_CONFIG_GLOBAL} "/dev/null")
set(ENV{GIT_CONFIG_NOSYSTEM} "1")
foreach(_v GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE)
  unset(ENV{${_v}})
endforeach()
foreach(_who AUTHOR COMMITTER)
  set(ENV{GIT_${_who}_NAME} "o3sdk test")
  set(ENV{GIT_${_who}_EMAIL} "test@example.invalid")
  set(ENV{GIT_${_who}_DATE} "2026-01-01T00:00:00Z")
endforeach()

find_program(GIT git)

function(fail what)
  message(FATAL_ERROR "gen_build_info_test: ${what}")
endfunction()

function(expect_eq what actual expected)
  if(NOT actual STREQUAL expected)
    fail("${what}: got '${actual}', expected '${expected}'")
  endif()
endfunction()

# git -C <dir> <args>: fatal on failure; the output (without the trailing newline) in GIT_OUT.
function(git dir)
  execute_process(COMMAND ${GIT} -C "${dir}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0)
    fail("git ${ARGN} in ${dir} failed (${rc}): ${err}")
  endif()
  set(GIT_OUT "${out}" PARENT_SCOPE)
endfunction()

# A repository with one commit whose only file holds `content` (so two repositories get two hashes).
function(make_repo dir content)
  file(MAKE_DIRECTORY "${dir}")
  git("${dir}" init -q)
  file(WRITE "${dir}/a.txt" "${content}\n")
  git("${dir}" add a.txt)
  git("${dir}" commit -q -m "first")
  git("${dir}" rev-parse --short=12 HEAD)
  set(HASH "${GIT_OUT}" PARENT_SCOPE)
endfunction()

# Runs the script (APP_SRC only when `app` is given), puts the header's text in HEADER and its defines in D_<NAME>.
function(run_gen kit app out)
  # Every argument quoted: a path may hold a ';'.
  if(app)
    execute_process(COMMAND ${CMAKE_COMMAND} "-DSRC=${kit}" "-DAPP_SRC=${app}" "-DOUT=${out}" -P ${GEN}
      RESULT_VARIABLE rc ERROR_VARIABLE err)
  else()
    execute_process(COMMAND ${CMAKE_COMMAND} "-DSRC=${kit}" "-DOUT=${out}" -P ${GEN} RESULT_VARIABLE rc ERROR_VARIABLE err)
  endif()
  if(NOT rc EQUAL 0)
    fail("the script failed (${rc}): ${err}")
  endif()
  file(READ "${out}" header)
  set(HEADER "${header}" PARENT_SCOPE)
  foreach(name BUILD_ID VERSION BUILD_LABEL APP_BUILD_ID APP_VERSION APP_BUILD_LABEL IMAGE_LABEL)
    if(NOT header MATCHES "#define O3SDK_${name} \"([^\"]*)\"\n")
      fail("O3SDK_${name} is not defined in:\n${header}")
    endif()
    set(D_${name} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  endforeach()
endfunction()

set(OUT "${WORK}/gen/o3sdk_build_info.h")

# ---- Outside git: "unknown", for both trees; one tree is one identity -------------------------------------------------------------
file(MAKE_DIRECTORY "${WORK}/nogit-kit" "${WORK}/nogit-app")
run_gen("${WORK}/nogit-kit" "" "${OUT}")
expect_eq("no git, one tree: kit id" "${D_BUILD_ID}" "unknown")
expect_eq("no git, one tree: kit version" "${D_VERSION}" "unknown")
expect_eq("no git, one tree: app id" "${D_APP_BUILD_ID}" "unknown")
expect_eq("no git, one tree: image label" "${D_IMAGE_LABEL}" "o3sdk unknown")
run_gen("${WORK}/nogit-kit" "${WORK}/nogit-kit/../nogit-kit" "${OUT}")
expect_eq("no git, the same directory spelled twice: image label" "${D_IMAGE_LABEL}" "o3sdk unknown")
run_gen("${WORK}/nogit-kit" "${WORK}/nogit-app" "${OUT}")
expect_eq("no git, two trees: image label" "${D_IMAGE_LABEL}" "unknown / o3sdk unknown")
expect_eq("no git, two trees: kit label" "${D_BUILD_LABEL}" "unknown")
expect_eq("no git, two trees: app label" "${D_APP_BUILD_LABEL}" "unknown")

# ---- The script refuses to run without its arguments -------------------------------------------------------------------------------
execute_process(COMMAND ${CMAKE_COMMAND} -DSRC=${WORK} -P ${GEN} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(rc EQUAL 0)
  fail("the script ran without -DOUT")
endif()

if(NOT GIT)
  message(STATUS "gen_build_info_test: git not found, the checkout cases are skipped")
  return()
endif()

# ---- A kit checkout and an app outside git -----------------------------------------------------------------------------------------
make_repo("${WORK}/kit" "kit")
set(KIT_HASH "${HASH}")
if(NOT KIT_HASH MATCHES "^[0-9a-f]+$")
  fail("odd kit hash '${KIT_HASH}'")
endif()
string(LENGTH "${KIT_HASH}" len)
expect_eq("the hash is 12 characters" "${len}" "12")

run_gen("${WORK}/kit" "" "${OUT}")
expect_eq("kit only: kit id" "${D_BUILD_ID}" "${KIT_HASH}")
expect_eq("kit only: kit label" "${D_BUILD_LABEL}" "${KIT_HASH}")
expect_eq("kit only: the app is the kit" "${D_APP_BUILD_ID}" "${KIT_HASH}")
expect_eq("kit only: image label" "${D_IMAGE_LABEL}" "o3sdk ${KIT_HASH}")
run_gen("${WORK}/kit" "${WORK}/kit" "${OUT}")
expect_eq("o3sdk's own app: image label" "${D_IMAGE_LABEL}" "o3sdk ${KIT_HASH}")
expect_eq("o3sdk's own app: app label" "${D_APP_BUILD_LABEL}" "${D_BUILD_LABEL}")

run_gen("${WORK}/kit" "${WORK}/nogit-app" "${OUT}")
expect_eq("app outside git: kit id" "${D_BUILD_ID}" "${KIT_HASH}")
expect_eq("app outside git: app id" "${D_APP_BUILD_ID}" "unknown")
expect_eq("app outside git: image label" "${D_IMAGE_LABEL}" "unknown / o3sdk ${KIT_HASH}")

# ---- A path with a ';' is one path ---------------------------------------------------------------------------------------------------
make_repo("${WORK}/odd;dir" "odd")
run_gen("${WORK}/odd;dir" "${WORK}/odd;dir" "${OUT}")
expect_eq("a path with a semicolon: kit id" "${D_BUILD_ID}" "${HASH}")
expect_eq("a path with a semicolon: image label" "${D_IMAGE_LABEL}" "o3sdk ${HASH}")
run_gen("${WORK}/kit" "${WORK}/odd;dir" "${OUT}")
expect_eq("an app path with a semicolon: app id" "${D_APP_BUILD_ID}" "${HASH}")
file(REMOVE_RECURSE "${WORK}/odd;dir")

# ---- An app repository of its own: the two hashes differ, the label has both, the app's is first ------------------------------------
make_repo("${WORK}/app" "app")
set(APP_HASH "${HASH}")
if(APP_HASH STREQUAL KIT_HASH)
  fail("the two repositories got one hash")
endif()
run_gen("${WORK}/kit" "${WORK}/app" "${OUT}")
expect_eq("app repository: kit id" "${D_BUILD_ID}" "${KIT_HASH}")
expect_eq("app repository: app id" "${D_APP_BUILD_ID}" "${APP_HASH}")
expect_eq("app repository: image label" "${D_IMAGE_LABEL}" "${APP_HASH} / o3sdk ${KIT_HASH}")

# ---- "-dirty" belongs to the tree that changed, an untracked file does not count ------------------------------------------------------
file(WRITE "${WORK}/app/untracked.txt" "x\n")
run_gen("${WORK}/kit" "${WORK}/app" "${OUT}")
expect_eq("untracked file: app id" "${D_APP_BUILD_ID}" "${APP_HASH}")
file(APPEND "${WORK}/app/a.txt" "changed\n")
run_gen("${WORK}/kit" "${WORK}/app" "${OUT}")
expect_eq("app changed: app id" "${D_APP_BUILD_ID}" "${APP_HASH}-dirty")
expect_eq("app changed: kit id" "${D_BUILD_ID}" "${KIT_HASH}")
expect_eq("app changed: image label" "${D_IMAGE_LABEL}" "${APP_HASH}-dirty / o3sdk ${KIT_HASH}")
file(APPEND "${WORK}/kit/a.txt" "changed\n")
run_gen("${WORK}/kit" "${WORK}/app" "${OUT}")
expect_eq("both changed: image label" "${D_IMAGE_LABEL}" "${APP_HASH}-dirty / o3sdk ${KIT_HASH}-dirty")
git("${WORK}/app" checkout -q -- a.txt)
git("${WORK}/kit" checkout -q -- a.txt)

# ---- A version tag: "<version> (<id>)" for the tree that has one ------------------------------------------------------------------------
git("${WORK}/app" tag v1.2.3)
run_gen("${WORK}/kit" "${WORK}/app" "${OUT}")
expect_eq("tagged app: version" "${D_APP_VERSION}" "v1.2.3")
expect_eq("tagged app: label" "${D_APP_BUILD_LABEL}" "v1.2.3 (${APP_HASH})")
string(SUBSTRING "${KIT_HASH}" 0 7 KIT_SHORT)  # describe --always --abbrev=7: the bare short hash
expect_eq("tagged app: the kit has no tag" "${D_VERSION}" "${KIT_SHORT}")
expect_eq("tagged app: so its label is the id" "${D_BUILD_LABEL}" "${KIT_HASH}")
expect_eq("tagged app: image label" "${D_IMAGE_LABEL}" "v1.2.3 (${APP_HASH}) / o3sdk ${KIT_HASH}")

# ---- Reproducible, and written only when the content changed ---------------------------------------------------------------------
set(FIRST "${HEADER}")
file(SHA256 "${OUT}" sha_before)
execute_process(COMMAND touch -t 200001010000 "${OUT}")  # POSIX touch: an old mtime a rewrite would replace
run_gen("${WORK}/kit" "${WORK}/app" "${OUT}")
expect_eq("a second run: same bytes" "${HEADER}" "${FIRST}")
file(SHA256 "${OUT}" sha_after)
expect_eq("a second run: same file" "${sha_after}" "${sha_before}")
file(TIMESTAMP "${OUT}" year "%Y")
expect_eq("a second run does not rewrite the header" "${year}" "2000")
file(APPEND "${WORK}/app/a.txt" "changed\n")
run_gen("${WORK}/kit" "${WORK}/app" "${OUT}")
file(TIMESTAMP "${OUT}" year "%Y")
if(year STREQUAL "2000")
  fail("a changed identity did not rewrite the header")
endif()

file(REMOVE_RECURSE "${WORK}")
message(STATUS "gen_build_info_test: ok")
