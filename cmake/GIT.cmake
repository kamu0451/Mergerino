# This script will set the following variables:
# GIT_HASH
#   If the git binary is found and the git work tree is intact, GIT_HASH is worked out using the `git rev-parse --short HEAD` command
#   The value of GIT_HASH can be overriden by defining the GIT_HASH environment variable
# GIT_COMMIT
#   If the git binary is found and the git work tree is intact, GIT_COMMIT is worked out using the `git rev-parse HEAD` command
#   The value of GIT_COMMIT can be overriden by defining the GIT_COMMIT environment variable
# GIT_RELEASE
#   If the git binary is found and the git work tree is intact, GIT_RELEASE is worked out using the `git describe` command
#   The value of GIT_RELEASE can be overriden by defining the GIT_RELEASE environment variable
# GIT_MODIFIED
#   If the git binary is found and the git work tree is intact, GIT_MODIFIED is worked out by checking if output of `git status --porcelain -z` command is empty
#   The value of GIT_MODIFIED cannot be overriden

find_package(Git)

set(GIT_HASH "GIT-REPOSITORY-NOT-FOUND")
set(GIT_COMMIT "GIT-REPOSITORY-NOT-FOUND")
set(GIT_RELEASE "${PROJECT_VERSION}")
set(GIT_MODIFIED 0)

if(DEFINED ENV{CHATTERINO_SKIP_GIT_GEN})
    return()
endif()

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --is-inside-work-tree
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE GIT_REPOSITORY_NOT_FOUND
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if(GIT_REPOSITORY_NOT_FOUND)
        set(GIT_REPOSITORY_FOUND 0)
    else()
        set(GIT_REPOSITORY_FOUND 1)
    endif()

    if(GIT_REPOSITORY_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_RELEASE
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain -z
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_MODIFIED_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif(GIT_REPOSITORY_FOUND)
endif(GIT_EXECUTABLE)

if(GIT_MODIFIED_OUTPUT)
    if(DEFINED ENV{CHATTERINO_REQUIRE_CLEAN_GIT})
        message(STATUS "git status --porcelain -z\n${GIT_MODIFIED_OUTPUT}")
        message(FATAL_ERROR "Git repository was expected to be clean, but modifications were found!")
    endif()

    set(GIT_MODIFIED 1)
endif()

if(DEFINED ENV{GIT_HASH})
    set(GIT_HASH "$ENV{GIT_HASH}")
endif()

if(DEFINED ENV{GIT_COMMIT})
    set(GIT_COMMIT "$ENV{GIT_COMMIT}")
endif()

if(DEFINED ENV{GIT_RELEASE})
    set(GIT_RELEASE "$ENV{GIT_RELEASE}")
endif()

# Auto-bump PATCH from the total commit count so every push monotonically
# advances the displayed version without a manual edit. MAJOR.MINOR stay
# under intentional control via the top-level project() call.
if(GIT_EXECUTABLE AND GIT_REPOSITORY_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_COUNT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_COMMIT_COUNT MATCHES "^[0-9]+$")
        set(PROJECT_VERSION_PATCH "${GIT_COMMIT_COUNT}")
        set(PROJECT_VERSION
            "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${GIT_COMMIT_COUNT}")
        message(STATUS "Auto-version: ${PROJECT_VERSION} (commit count ${GIT_COMMIT_COUNT})")
    endif()

    # Force a reconfigure when HEAD moves or the followed branch ref advances,
    # so the cached auto-version doesn't go stale across ninja invocations
    # (.local-build.bat reuses the existing CMake config by design).
    if(EXISTS "${CMAKE_SOURCE_DIR}/.git/HEAD")
        configure_file("${CMAKE_SOURCE_DIR}/.git/HEAD"
                       "${CMAKE_BINARY_DIR}/git-head-stamp" COPYONLY)
        file(STRINGS "${CMAKE_SOURCE_DIR}/.git/HEAD" _GIT_HEAD_CONTENTS LIMIT_COUNT 1)
        if(_GIT_HEAD_CONTENTS MATCHES "^ref: (.*)$")
            set(_GIT_REF "${CMAKE_MATCH_1}")
            if(EXISTS "${CMAKE_SOURCE_DIR}/.git/${_GIT_REF}")
                configure_file("${CMAKE_SOURCE_DIR}/.git/${_GIT_REF}"
                               "${CMAKE_BINARY_DIR}/git-ref-stamp" COPYONLY)
            endif()
        endif()
    endif()
endif()

message(STATUS "Injected git values: ${GIT_COMMIT} (${GIT_RELEASE}) modified: ${GIT_MODIFIED}")
