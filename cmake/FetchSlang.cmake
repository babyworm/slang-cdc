# FetchSlang.cmake — Download and configure slang as a dependency
include(FetchContent)

set(SLANG_VERSION "v10.0" CACHE STRING "slang version to fetch")

FetchContent_Declare(
    slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG        ${SLANG_VERSION}
    GIT_SHALLOW    ON
)

# Disable slang extras we don't need
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(slang)
