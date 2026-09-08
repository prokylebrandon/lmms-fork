IF(LMMS_HAVE_VST3)
	include_directories(SYSTEM ${LMMS_VST3_SDK_ROOT})
	INCLUDE(BuildPlugin)
	BUILD_PLUGIN(vst3instrument Vst3Instrument.cpp Vst3InsView.cpp Vst3Instrument.h Vst3InsView.h
		MOCFILES Vst3Instrument.h Vst3InsView.h)
	target_link_libraries(vst3instrument sdk_hosting)
ENDIF(LMMS_HAVE_VST3)
