if(NOT LMMS_HAVE_VST3)
    return()
endif()

include(BuildPlugin)

BUILD_PLUGIN(prestige
    Prestige.cpp
    Vst3ParameterModel.cpp
    Vst3ParameterWindow.cpp
    MOCFILES Prestige.h Vst3ParameterModel.h Vst3ParameterWindow.h
    EMBEDDED_RESOURCES logo.svg
)

target_link_libraries(prestige vst3base)
target_include_directories(prestige PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/Vst3Base"
    "${CMAKE_SOURCE_DIR}/plugins/Vst3Base/vst3sdk"
)
