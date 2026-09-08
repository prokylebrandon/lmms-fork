IF(LMMS_HAVE_VST3)
	include_directories(SYSTEM ${LMMS_VST3_SDK_ROOT})
	INCLUDE(BuildPlugin)
	BUILD_PLUGIN(vst3effect Vst3Effect.cpp Vst3FxControls.cpp Vst3FxControlDialog.cpp Vst3Effect.h Vst3FxControls.h Vst3FxControlDialog.h
		MOCFILES Vst3Effect.h Vst3FxControls.h Vst3FxControlDialog.h)
	target_link_libraries(vst3effect sdk_hosting)
ENDIF(LMMS_HAVE_VST3)
