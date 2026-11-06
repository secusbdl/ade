cmake_minimum_required(VERSION 3.24)
project(AdeBrowserProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Test)
find_package(Qt6 REQUIRED COMPONENTS Qml)
qt_add_executable(ade-browser-probe main.cpp ../../src/browser/browserwidget.cpp ../../src/browser/browserwidget.h)
target_include_directories(ade-browser-probe PRIVATE ../../src)
target_include_directories(ade-browser-probe SYSTEM PRIVATE ../../third_party/webview2/include)
target_link_libraries(ade-browser-probe PRIVATE Qt6::Widgets ole32)
target_compile_options(ade-browser-probe PRIVATE -Wall -Wextra)
add_custom_command(TARGET ade-browser-probe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/x64/WebView2Loader.dll"
    "$<TARGET_FILE_DIR:ade-browser-probe>/WebView2Loader.dll")
qt_add_executable(ade-browser-tests ../../tests/test_browser.cpp ../../src/browser/browserwidget.cpp ../../src/browser/browserwidget.h)
target_include_directories(ade-browser-tests PRIVATE ../../src)
target_include_directories(ade-browser-tests SYSTEM PRIVATE ../../third_party/webview2/include)
target_link_libraries(ade-browser-tests PRIVATE Qt6::Widgets Qt6::Network Qt6::Test ole32)
target_compile_options(ade-browser-tests PRIVATE -Wall -Wextra)
add_custom_command(TARGET ade-browser-tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/x64/WebView2Loader.dll"
    "$<TARGET_FILE_DIR:ade-browser-tests>/WebView2Loader.dll")
qt_add_executable(ade-browser-tool-tests ../../tests/test_browser_tools.cpp
    ../../src/browser/browserwidget.cpp ../../src/browser/browserwidget.h
    ../../src/browser/BrowserToolBridge.cpp ../../src/browser/BrowserToolBridge.h)
target_include_directories(ade-browser-tool-tests PRIVATE ../../src)
target_include_directories(ade-browser-tool-tests SYSTEM PRIVATE ../../third_party/webview2/include)
target_link_libraries(ade-browser-tool-tests PRIVATE Qt6::Widgets Qt6::Network Qt6::Test ole32)
target_compile_options(ade-browser-tool-tests PRIVATE -Wall -Wextra)
add_custom_command(TARGET ade-browser-tool-tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/x64/WebView2Loader.dll"
    "$<TARGET_FILE_DIR:ade-browser-tool-tests>/WebView2Loader.dll")
qt_add_executable(ade-js-runtime-probe js-runtime-probe.cpp)
target_link_libraries(ade-js-runtime-probe PRIVATE Qt6::Qml)
qt_add_executable(ade-downloads-tests ../../tests/test_downloads.cpp ../../src/ui/DownloadsPage.cpp ../../src/ui/DownloadsPage.h
    ../../src/browser/browserwidget.cpp ../../src/browser/browserwidget.h)
target_include_directories(ade-downloads-tests PRIVATE ../../src)
target_include_directories(ade-downloads-tests SYSTEM PRIVATE ../../third_party/webview2/include)
target_link_libraries(ade-downloads-tests PRIVATE Qt6::Widgets Qt6::Network Qt6::Test ole32)
target_compile_options(ade-downloads-tests PRIVATE -Wall -Wextra)
add_custom_command(TARGET ade-downloads-tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/x64/WebView2Loader.dll" "$<TARGET_FILE_DIR:ade-downloads-tests>/WebView2Loader.dll")
