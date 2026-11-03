find_package(Qt6 6.5 CONFIG REQUIRED COMPONENTS Quick QuickControls2 Multimedia)
qt_standard_project_setup()
qt_add_executable(aiplayer_app
    src/main.cpp
    src/video/VideoItem.cpp
    src/video/VideoItem.h
    src/ui/PlaybackController.cpp
    src/ui/PlaybackController.h
    src/ui/MediaLibrary.cpp
    src/ui/MediaLibrary.h
)
target_include_directories(aiplayer_app PRIVATE src)
target_link_libraries(aiplayer_app PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia aiplayer_audio_output aiplayer_video_output)
qt_add_resources(aiplayer_app "ui" PREFIX "/ui" BASE "qml"
    FILES qml/Main.qml qml/IconButton.qml qml/Glyph.qml qml/SeekSlider.qml qml/PlaybackSeekSlider.qml)
set_target_properties(aiplayer_app PROPERTIES OUTPUT_NAME AiPlayer
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.aiplayer.desktop"
    MACOSX_BUNDLE_BUNDLE_NAME "AiPlayer")

option(AIPLAYER_BUILD_UI_TESTS "Build actual QML playback interaction tests" OFF)
if(AIPLAYER_BUILD_UI_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/tests/PlaybackUiTests.cpp")
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Test)
    qt_add_executable(aiplayer_ui_tests
        ../tests/PlaybackUiTests.cpp
        src/video/VideoItem.cpp src/video/VideoItem.h
        src/ui/PlaybackController.cpp src/ui/PlaybackController.h
        src/ui/MediaLibrary.cpp src/ui/MediaLibrary.h)
    target_include_directories(aiplayer_ui_tests PRIVATE src)
    target_link_libraries(aiplayer_ui_tests PRIVATE Qt6::Test Qt6::Quick
        Qt6::QuickControls2 Qt6::Multimedia aiplayer_audio_output aiplayer_video_output)
    qt_add_resources(aiplayer_ui_tests "ui" PREFIX "/ui" BASE "qml"
        FILES qml/Main.qml qml/IconButton.qml qml/Glyph.qml qml/SeekSlider.qml qml/PlaybackSeekSlider.qml)
    set_target_properties(aiplayer_ui_tests PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.aiplayer.interactiontests")
endif()

find_package(inspireface CONFIG REQUIRED)
# SDK 和模型跟随应用，不依赖启动工作目录或 Conan 缓存中的运行时路径。
if(AIPLAYER_BUILD_QT)
    foreach(app_target aiplayer_app aiplayer_ui_tests)
        if(TARGET ${app_target})
            set_property(TARGET ${app_target} APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
            add_custom_command(TARGET ${app_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:${app_target}>/Contents/Frameworks" "$<TARGET_BUNDLE_DIR:${app_target}>/Contents/Resources"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${inspireface_PACKAGE_FOLDER_RELEASE}/lib/libInspireFace.dylib" "$<TARGET_BUNDLE_DIR:${app_target}>/Contents/Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${inspireface_PACKAGE_FOLDER_RELEASE}/res/Pikachu" "$<TARGET_BUNDLE_DIR:${app_target}>/Contents/Resources")
        endif()
    endforeach()
endif()
