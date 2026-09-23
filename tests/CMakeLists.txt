include(CTest)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

set(LMMS_TESTS
	src/core/ArrayVectorTest.cpp
	src/core/AudioBufferTest.cpp
	src/core/AutomatableModelTest.cpp
	src/core/MathTest.cpp
	src/core/ProjectVersionTest.cpp
	src/core/RelativePathsTest.cpp
	src/core/TimelineTest.cpp
	src/tracks/AutomationTrackTest.cpp
)

if(LMMS_HAVE_VST3)
	list(APPEND LMMS_TESTS src/plugins/Vst3BaseTest.cpp)
endif()

foreach(LMMS_TEST_SRC IN LISTS LMMS_TESTS)
	# TODO CMake 3.20: Use cmake_path
	get_filename_component(LMMS_TEST_NAME ${LMMS_TEST_SRC} NAME_WE)

	add_executable(${LMMS_TEST_NAME} ${LMMS_TEST_SRC})
	add_test(NAME ${LMMS_TEST_NAME} COMMAND ${LMMS_TEST_NAME})

	# TODO CMake 3.12: Propagate usage requirements by linking to lmmsobjs
	target_include_directories(${LMMS_TEST_NAME} PRIVATE $<TARGET_PROPERTY:lmmsobjs,INCLUDE_DIRECTORIES>)

	target_static_libraries("${LMMS_TEST_NAME}" PRIVATE lmmsobjs)
	target_link_libraries(${LMMS_TEST_NAME} PRIVATE
		${QT_LIBRARIES}
		${QT_QTTEST_LIBRARY}
	)

	target_compile_features(${LMMS_TEST_NAME} PRIVATE cxx_std_20)
	target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_TESTING)
endforeach()

if(LMMS_HAVE_VST3)
	target_compile_definitions(Vst3BaseTest PRIVATE LMMS_HAVE_VST3)
	target_link_libraries(Vst3BaseTest PRIVATE vst3base)
	target_include_directories(Vst3BaseTest PRIVATE
		"${CMAKE_SOURCE_DIR}/plugins/Vst3Base"
		"${CMAKE_SOURCE_DIR}/plugins/Vst3Base/vst3sdk")

if(WIN32)
    set_tests_properties(Vst3BaseTest PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:vst3base>")
elseif(APPLE)
    set_tests_properties(Vst3BaseTest PROPERTIES
        ENVIRONMENT "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:vst3base>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set_tests_properties(Vst3BaseTest PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:vst3base>:$ENV{LD_LIBRARY_PATH}")
endif()
endif()
