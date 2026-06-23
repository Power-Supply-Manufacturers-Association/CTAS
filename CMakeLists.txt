cmake_minimum_required(VERSION 3.15)
project(CTAS LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# --- nlohmann/json (header-only). Idempotent by name when a parent already declared `json`. ---
include(FetchContent)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG tags/v3.11.3)
FetchContent_MakeAvailable(json)

# --- Core library: ctas_to_cias (the CTAS family's CIAS lowering). ---
# CTAS is otherwise a schema-only repo; this lightweight converter lowers a controller's ideal
# `behavioral` control law (e.g. the synchronous-rectifier scheme) to a CIAS network of AAS comparators.
add_library(CTAS STATIC src/CtasConverter.cpp)
target_include_directories(CTAS PUBLIC
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/../PEAS/src")    # Fidelity.hpp
target_link_libraries(CTAS PUBLIC nlohmann_json::nlohmann_json)
