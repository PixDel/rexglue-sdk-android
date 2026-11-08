# Optional frame generation (Android): Lossless Scaling Frame Generation through lsfg-vk's
# framegen library (MIT, GameNative/lsfg-vk-android). Its shaders are not part of it: they are
# read at runtime from the user's own Lossless.dll and translated from DXBC to SPIR-V.
#
#   -DREXGLUE_LSFG_DIR=<checkout of lsfg-vk-android with its submodules>
#
# Defines the target rex_lsfg and REX_HAS_LSFG=1 for users of it. Everything is built under
# lsfg_* target names: rexglue has its own dxbc and pe libraries.

if(NOT ANDROID OR NOT REXGLUE_LSFG_DIR)
    return()
endif()
if(NOT EXISTS "${REXGLUE_LSFG_DIR}/framegen/public/lsfg_3_1.hpp" OR
   NOT EXISTS "${REXGLUE_LSFG_DIR}/thirdparty/dxbc/CMakeLists.txt")
    message(WARNING "REXGLUE_LSFG_DIR (${REXGLUE_LSFG_DIR}) is not an lsfg-vk-android checkout "
                    "with submodules: frame generation off")
    return()
endif()

set(_lsfg "${REXGLUE_LSFG_DIR}")
set(_lsfg_vk_headers "${REXGLUE_ROOT}/thirdparty/vulkan-headers/include")

# volk: lsfg's Vulkan loader (global function pointers; hidden so they never leave our .so).
add_library(lsfg_volk STATIC "${_lsfg}/thirdparty/volk/volk.c")
target_include_directories(lsfg_volk PUBLIC "${_lsfg}/thirdparty/volk" "${_lsfg_vk_headers}")
target_compile_options(lsfg_volk PRIVATE -fPIC -fvisibility=hidden)
target_compile_definitions(lsfg_volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)

# DXVK's DXBC -> SPIR-V translator (PancakeTAS/dxbc, zlib).
file(GLOB _lsfg_dxbc_sources
    "${_lsfg}/thirdparty/dxbc/src/dxbc/*.cpp"
    "${_lsfg}/thirdparty/dxbc/src/spirv/*.cpp"
    "${_lsfg}/thirdparty/dxbc/src/util/*.cpp")
add_library(lsfg_dxbc STATIC ${_lsfg_dxbc_sources})
set_target_properties(lsfg_dxbc PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(lsfg_dxbc SYSTEM PUBLIC
    "${_lsfg}/thirdparty/dxbc/include/dxbc" "${_lsfg}/thirdparty/dxbc/include/spirv"
    "${_lsfg}/thirdparty/dxbc/include/util" "${_lsfg}/thirdparty/dxbc/include/dxvk"
    "${_lsfg_vk_headers}")
target_compile_options(lsfg_dxbc PRIVATE -fPIC -fvisibility=hidden -w)

# pe-parse: reads the shader resources out of Lossless.dll.
add_library(lsfg_peparse STATIC
    "${_lsfg}/thirdparty/pe-parse/pe-parser-library/src/buffer.cpp"
    "${_lsfg}/thirdparty/pe-parse/pe-parser-library/src/parse.cpp"
    "${_lsfg}/thirdparty/pe-parse/pe-parser-library/src/unicode_codecvt.cpp")
target_include_directories(lsfg_peparse PUBLIC
    "${_lsfg}/thirdparty/pe-parse/pe-parser-library/include")
target_compile_options(lsfg_peparse PRIVATE -fPIC -fvisibility=hidden -w)

# The framegen library itself.
file(GLOB _lsfg_framegen_sources
    "${_lsfg}/framegen/src/common/*.cpp" "${_lsfg}/framegen/src/config/*.cpp"
    "${_lsfg}/framegen/src/core/*.cpp" "${_lsfg}/framegen/src/pool/*.cpp"
    "${_lsfg}/framegen/src/*.cpp"
    "${_lsfg}/framegen/v3.1_src/core/*.cpp" "${_lsfg}/framegen/v3.1_src/pool/*.cpp"
    "${_lsfg}/framegen/v3.1_src/shaders/*.cpp" "${_lsfg}/framegen/v3.1_src/utils/*.cpp"
    "${_lsfg}/framegen/v3.1_src/*.cpp"
    "${_lsfg}/framegen/v3.1p_src/core/*.cpp" "${_lsfg}/framegen/v3.1p_src/pool/*.cpp"
    "${_lsfg}/framegen/v3.1p_src/shaders/*.cpp" "${_lsfg}/framegen/v3.1p_src/utils/*.cpp"
    "${_lsfg}/framegen/v3.1p_src/*.cpp")
add_library(lsfg_framegen STATIC ${_lsfg_framegen_sources})
set_target_properties(lsfg_framegen PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(lsfg_framegen
    PUBLIC "${_lsfg}/framegen/include" "${_lsfg}/framegen/public"
    PRIVATE "${_lsfg}/framegen/v3.1_include" "${_lsfg}/framegen/v3.1p_include")
target_link_libraries(lsfg_framegen PUBLIC lsfg_volk log)
target_compile_options(lsfg_framegen PRIVATE -fPIC -w)

# Shader extraction (lsfg-vk's src/extract) plus the Adreno mipmaps shader replacement.
add_library(rex_lsfg STATIC
    "${_lsfg}/src/extract/extract.cpp"
    "${_lsfg}/src/extract/trans.cpp"
    "${REXGLUE_ROOT}/src/ui/vulkan/lsfg_config_stub.cpp")
set_target_properties(rex_lsfg PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(rex_lsfg
    PUBLIC "${_lsfg}/include" "${_lsfg}/src" "${_lsfg}")
target_link_libraries(rex_lsfg PUBLIC lsfg_framegen lsfg_dxbc lsfg_peparse)
target_compile_definitions(rex_lsfg INTERFACE REX_HAS_LSFG=1)
target_compile_options(rex_lsfg PRIVATE -fPIC -w)
message(STATUS "  Frame generation (lsfg): ${_lsfg}")
