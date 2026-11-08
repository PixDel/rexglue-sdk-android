# rexui

# Core UI sources
set(REXUI_CORE_SOURCES
    graphics_upload_buffer_pool.cpp
    graphics_util.cpp
    image_decode.cpp
    imgui_dialog.cpp
    imgui_drawer.cpp
    immediate_drawer.cpp
    keybinds.cpp
    menu_item.cpp
    presenter.cpp
    renderdoc_api.cpp
    sdl_virtual_key.cpp
    window.cpp
    window_sdl.cpp
    windowed_app.cpp
    windowed_app_context.cpp
    windowed_app_context_sdl.cpp
)

# Optional overlay dialogs
set(REXUI_OVERLAY_SOURCES
    overlay/achievement_icon_cache.cpp
    overlay/achievement_toast.cpp
    overlay/achievements_overlay.cpp
    overlay/debug_overlay.cpp
    overlay/console_overlay.cpp
    overlay/console_commands.cpp
    overlay/settings_overlay.cpp
)

# Vulkan UI backend
set(REXUI_VULKAN_SOURCES
    vulkan/linked_type_descriptor_set_allocator.cpp
    vulkan/vulkan_moltenvk.cpp
    vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_tools_context.cpp
    vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_immediate_drawer.cpp
    vulkan/vulkan_instance.cpp
    vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_presenter.cpp
    vulkan/vulkan_presenter_frame_generation.cpp
    vulkan/vulkan_provider.cpp
    vulkan/vulkan_submission_tracker.cpp
    vulkan/vulkan_upload_buffer_pool.cpp
    vulkan/vulkan_util.cpp
)

# D3D12 UI backend
if(REXGLUE_USE_D3D12)
    set(REXUI_D3D12_SOURCES
        d3d12/d3d12_cpu_descriptor_pool.cpp
        d3d12/d3d12_descriptor_heap_pool.cpp
        d3d12/d3d12_immediate_drawer.cpp
        d3d12/d3d12_presenter.cpp
        d3d12/d3d12_provider.cpp
        d3d12/d3d12_submission_tracker.cpp
        d3d12/d3d12_upload_buffer_pool.cpp
        d3d12/d3d12_util.cpp
    )
endif()

# Platform-specific sources (windowing itself is SDL3, platform-independent)
if(WIN32)
    set(REXUI_PLATFORM_SOURCES
        surface_win.cpp
    )
elseif(APPLE)
    set(REXUI_PLATFORM_SOURCES
        surface_mac.cpp
    )
elseif(ANDROID)
    # AndroidNativeWindowSurface is header-only (rex/ui/surface_android.h).
    set(REXUI_PLATFORM_SOURCES)
else()
    set(REXUI_PLATFORM_SOURCES
        surface_gnulinux.cpp
    )
endif()

# Combine sources
set(REXUI_SOURCES
    ${REXUI_CORE_SOURCES}
    ${REXUI_OVERLAY_SOURCES}
    ${REXUI_PLATFORM_SOURCES}
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXUI_SOURCES ${REXUI_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXUI_SOURCES ${REXUI_D3D12_SOURCES})
endif()

add_library(rexui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rexui)

rex_target_pch(rexui <rex/math.h>)

target_include_directories(rexui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexui PRIVATE
    ${PROJECT_BINARY_DIR}/include  # Generated macOS Vulkan path data
    ${REXGLUE_ROOT}/src  # For ui/shaders/bytecode includes
    ${REXGLUE_ROOT}/thirdparty/stb  # stb_image.h for icon decoding
)

target_link_libraries(rexui PUBLIC
    rexcore
    renderdoc
    imgui::imgui
    SDL3::SDL3
)
target_link_libraries(rexui PRIVATE utf8cpp)

if(REXGLUE_FIDELITYFX_SOURCE_DIR AND EXISTS "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include")
    target_include_directories(rexui PRIVATE
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include"
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/ffx-api/include"
    )
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_SDK=1)
elseif(ANDROID)
    # CAS and FSR 1 run from the prebuilt shaders in shaders/vulkan_spirv and need no SDK headers
    # (only the temporal FSR 2/3 do, behind REX_HAS_FIDELITYFX_RUNTIME): the sharpening and
    # upscaling choices for the tablets' small 3D pictures.
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_SDK=1)
endif()

set(REXUI_HAS_FIDELITYFX_RUNTIME OFF)
if(TARGET amd_fidelityfx_vk)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_vk)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(TARGET amd_fidelityfx_dx12)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_dx12)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(REXUI_HAS_FIDELITYFX_RUNTIME)
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_RUNTIME=1)
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexui PUBLIC
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
    )
endif()

# Platform-specific dependencies
if(ANDROID)
    # Android: the window and Vulkan surface come from SDL3 / ANativeWindow; no X11 or Wayland.
    target_link_libraries(rexui PUBLIC android log)
    # libadrenotools: load a custom Vulkan driver (Mesa Turnip) instead of the system one
    # (vulkan_driver). Its hook libraries must end up in the APK's native library folder.
    if(NOT TARGET adrenotools)
        add_subdirectory(${REXGLUE_ROOT}/thirdparty/libadrenotools
                         ${CMAKE_BINARY_DIR}/libadrenotools EXCLUDE_FROM_ALL)
    endif()
    target_link_libraries(rexui PRIVATE adrenotools)
    # Optional frame generation (lsfg-vk framegen, REXGLUE_LSFG_DIR).
    include(${REXGLUE_ROOT}/cmake/rexglue_lsfg.cmake)
    if(TARGET rex_lsfg)
        target_link_libraries(rexui PRIVATE rex_lsfg)
    endif()
    add_dependencies(rexui main_hook file_redirect_hook gsl_alloc_hook hook_impl)
    target_compile_definitions(rexui PRIVATE REX_HAS_ADRENOTOOLS=1)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    # Required, not optional: vulkan/api.h pulls in <wayland-client.h> on every
    # Linux build.
    pkg_check_modules(WAYLAND REQUIRED wayland-client)

    target_include_directories(rexui PUBLIC
        ${X11_XCB_INCLUDE_DIRS}
        ${WAYLAND_INCLUDE_DIRS}
    )
    target_link_libraries(rexui PUBLIC
        ${X11_XCB_LIBRARIES}
        ${WAYLAND_LIBRARIES}
    )
elseif(APPLE)
    target_link_libraries(rexui PRIVATE "-framework CoreFoundation")
endif()

if(WIN32)
    target_link_libraries(rexui PUBLIC dxgi dxguid)
endif()

# D3D12 backend dependencies
if(REXGLUE_USE_D3D12)
    target_link_libraries(rexui PUBLIC dxc-headers)
endif()

