# cmake/version.cmake
#
# Resolves RTOS_VERSION_MAJOR / MINOR / PATCH from a git tag.
#
# Tag format expected: v<major>.<minor>.<patch>  (e.g. v1.2.3)
#
# If git is unavailable or no matching tag exists, falls back to "0.0.0-dev".
# The three numeric variables and RTOS_VERSION (the full string) are set in
# the calling scope.

find_package(Git QUIET)

set(_rtos_fallback_version "0.0.0-dev")

if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --match "v*" --abbrev=0
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_tag
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    set(_git_tag "")
endif()

if(_git_tag MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(RTOS_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(RTOS_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(RTOS_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(RTOS_VERSION "${RTOS_VERSION_MAJOR}.${RTOS_VERSION_MINOR}.${RTOS_VERSION_PATCH}")
else()
    message(STATUS "quartz: no version tag found, using ${_rtos_fallback_version}")
    set(RTOS_VERSION_MAJOR "0")
    set(RTOS_VERSION_MINOR "0")
    set(RTOS_VERSION_PATCH "0")
    set(RTOS_VERSION "${_rtos_fallback_version}")
endif()

message(STATUS "quartz version: ${RTOS_VERSION}")
