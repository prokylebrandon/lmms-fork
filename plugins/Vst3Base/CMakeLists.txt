if(NOT LMMS_HAVE_VST3)
    return()
endif()

include(BuildPlugin)

# ---- VST3 SDK submodule ----
set(VST3SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/vst3sdk")

# Sources we need from the SDK (host-side only; no VSTGUI, no samples)
set(VST3SDK_SOURCES
    # pluginterfaces — pure headers, but these have .cpp companions
    ${VST3SDK_DIR}/pluginterfaces/base/conststringtable.cpp
    ${VST3SDK_DIR}/pluginterfaces/base/coreiids.cpp
    ${VST3SDK_DIR}/pluginterfaces/base/funknown.cpp
    ${VST3SDK_DIR}/pluginterfaces/base/ustring.cpp

    # base/source
    ${VST3SDK_DIR}/base/source/baseiids.cpp
    ${VST3SDK_DIR}/base/source/fbuffer.cpp
    ${VST3SDK_DIR}/base/source/fdebug.cpp
    ${VST3SDK_DIR}/base/source/fdynlib.cpp
    ${VST3SDK_DIR}/base/source/fobject.cpp
    ${VST3SDK_DIR}/base/source/fstreamer.cpp
    ${VST3SDK_DIR}/base/source/fstring.cpp
    ${VST3SDK_DIR}/base/source/timer.cpp
    ${VST3SDK_DIR}/base/source/updatehandler.cpp

    # vst utility (needed by hosting/module.cpp)
    ${VST3SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/utility/systemtime.cpp

    # vst hosting layer
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3SDK_DIR}/public.sdk/source/vst/hosting/processdata.cpp

    # common
    ${VST3SDK_DIR}/public.sdk/source/common/memorystream.cpp
)

# Platform-specific module loader
if(LMMS_BUILD_LINUX)
    list(APPEND VST3SDK_SOURCES
        ${VST3SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
elseif(LMMS_BUILD_WIN32)
    list(APPEND VST3SDK_SOURCES
        ${VST3SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
elseif(LMMS_BUILD_APPLE)
    list(APPEND VST3SDK_SOURCES
        ${VST3SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
endif()

# ---- vst3base shared library ----
# Built as SHARED (matching vstbase) so future plugins (Prestige, Vst3Effect)
# can link against it without duplicating SDK object code.
BUILD_PLUGIN(vst3base
    Vst3PluginInstance.cpp
    ${VST3SDK_SOURCES}
    EXPORT_BASE_NAME vst3base
    LINK SHARED
)

target_include_directories(vst3base
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}   # exposes Vst3PluginInstance.h et al.
        ${VST3SDK_DIR}              # exposes pluginterfaces/, public.sdk/, base/
    PRIVATE
        ${VST3SDK_DIR}/base/thread  # needed by fdebug.cpp on some platforms
)

# The SDK uses C++17 <filesystem> on Linux
target_compile_features(vst3base PRIVATE cxx_std_17)

# SDK silence: the SDK has internal warnings we can't fix
target_compile_options(vst3base PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-w>
)

# Link against dl on Linux (needed by module_linux.cpp).
# stdc++fs is only required on GCC < 9; later GCC and Clang ship filesystem
# in the main runtime library.  Using $<LINK_ONLY:> with a generator
# expression avoids linking it on toolchains that don't need it.
if(LMMS_BUILD_LINUX)
    target_link_libraries(vst3base dl)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
        target_link_libraries(vst3base stdc++fs)
    endif()
endif()

if(LMMS_BUILD_APPLE)
    target_link_libraries(vst3base "-framework CoreFoundation")
endif()
