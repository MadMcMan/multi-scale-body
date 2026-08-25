cmake_minimum_required(VERSION 3.15)

if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NAME MultiScaleBody)
set(PROJECT_VERSION_NUMBER "0.1.0")
project(${NAME} VERSION ${PROJECT_VERSION_NUMBER})

set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Path to deps directory")

if(NOT EXISTS "${DEPS_DIR}/DPF/CMakeLists.txt")
    message(FATAL_ERROR "DEPS_DIR must point to a directory containing DPF/. Got: ${DEPS_DIR}")
endif()

# Tell LVGL where to find its config
set(LV_BUILD_CONF_PATH "${PROJECT_SOURCE_DIR}/plugins/MultiScaleBody/src/lv_conf.h" CACHE PATH "")
set(LV_CONF_PATH "${LV_BUILD_CONF_PATH}" CACHE PATH "" FORCE)

set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)

add_subdirectory("${DEPS_DIR}/DPF" "${CMAKE_BINARY_DIR}/_dph" EXCLUDE_FROM_ALL)
add_subdirectory("${DEPS_DIR}/lvgl" "${CMAKE_BINARY_DIR}/_lvgl" EXCLUDE_FROM_ALL)
if(TARGET lvgl)
    # lv_blend_helium.S fails to assemble outside ARM builds with CMSIS; drop it
    # always, plus the whole helium/neon source dirs on non-ARM targets.
    get_target_property(_lvgl_sources lvgl SOURCES)
    if(_lvgl_sources)
        list(FILTER _lvgl_sources EXCLUDE REGEX "lv_blend_helium\\.S$")
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
        if(NOT _proc MATCHES "arm|aarch64")
            list(FILTER _lvgl_sources EXCLUDE REGEX "[\\\\/](helium|neon)[\\\\/]")
        endif()
        set_property(TARGET lvgl PROPERTY SOURCES "${_lvgl_sources}")
    endif()
    set_target_properties(lvgl PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(lvgl PRIVATE plugins/MultiScaleBody/src/lv_global_provider.c)
endif()
dpf_add_plugin(${NAME}
  TARGETS jack vst3 clap lv2
  FILES_DSP
      plugins/MultiScaleBody/src/PluginMultiScaleBody.cpp
      plugins/MultiScaleBody/src/MultiScaleBodyEngine.cpp
  FILES_UI
      plugins/MultiScaleBody/src/PluginUI.cpp
      "${DEPS_DIR}/dpf-widgets/generic/LVGL.cpp"
)

if(WIN32)
    target_link_options(${NAME}-ui PRIVATE -Wl,--allow-shlib-undefined)
endif()

# Our lv_global_provider.c owns lv_global_default(); exclude the thread_local
# (NULL-on-first-use) fallback at the bottom of dpf-widgets/generic/LVGL.cpp,
# otherwise both symbols link and the fallback wins -> memset(NULL) crash.
target_compile_definitions(${NAME}-ui PRIVATE SNAP_HAVE_LV_GLOBAL_PROVIDER)

target_include_directories(${NAME} PUBLIC
    plugins/MultiScaleBody/src
    "${DEPS_DIR}/dpf-widgets/generic"
)

target_link_libraries(${NAME} PUBLIC lvgl::lvgl $<$<BOOL:${WIN32}>:winmm>)

if(WIN32)
    target_compile_definitions(${NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${NAME}-dsp PRIVATE -Wall -Wextra -Wno-multichar -Wno-cpp)
    target_compile_options(${NAME}-ui  PRIVATE -Wall -Wextra -Wno-multichar -Wno-cpp)
    set_source_files_properties(
        "${DEPS_DIR}/dpf-widgets/generic/LVGL.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-error")
endif()
