if(NOT LMMS_HAVE_VST3)
    return()
endif()

include(BuildPlugin)

BUILD_PLUGIN(vst3effect
    Vst3Effect.cpp
    Vst3EffectControls.cpp
    Vst3EffectControlDialog.cpp
    Vst3SubPluginFeatures.cpp
    MOCFILES Vst3Effect.h Vst3EffectControls.h Vst3EffectControlDialog.h
    EMBEDDED_RESOURCES logo.svg
)

target_link_libraries(vst3effect vst3base)
target_include_directories(vst3effect PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/Vst3Base"
    "${CMAKE_SOURCE_DIR}/plugins/Vst3Base/vst3sdk"
)

# NOTE (batch 1, unverified): the MOCFILES line above lists every header in
# this target defensively, mirroring prestige-vst3.md's Prestige example.
# Vst3EffectControlDialog.h now genuinely needs its entry (Task 3, batch 2
# added Q_OBJECT + a private slot to it); the other two remain defensive
# and may be trimmable once BuildPlugin.cmake's actual MOC-file
# requirements are confirmed.
