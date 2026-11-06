cmake_minimum_required(VERSION 3.25)
project(AdeRuntimeAppsProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_LINK_SEARCH_END_STATIC TRUE)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Sql Test Concurrent ZlibPrivate)
add_compile_options(-Wall -Wextra -Wpedantic)
add_link_options(-static -static-libgcc -static-libstdc++)
set(ADE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(GLOB CORE_SOURCES "${ADE_ROOT}/src/core/*.cpp" "${ADE_ROOT}/src/core/*.h")
file(GLOB_RECURSE ATTACHMENT_SOURCES "${ADE_ROOT}/src/attachments/*.cpp" "${ADE_ROOT}/src/attachments/*.h")
file(GLOB APP_SOURCES "${ADE_ROOT}/src/apps/*.cpp" "${ADE_ROOT}/src/apps/*.h")
file(GLOB BROWSER_SOURCES "${ADE_ROOT}/src/browser/*.cpp" "${ADE_ROOT}/src/browser/*.h")
add_library(ade_probe_host STATIC ${CORE_SOURCES} ${ATTACHMENT_SOURCES} ${APP_SOURCES} ${BROWSER_SOURCES}
    "${ADE_ROOT}/src/network/NetworkBroker.cpp" "${ADE_ROOT}/src/network/NetworkBroker.h"
    "${ADE_ROOT}/src/runtime/AgentToolHost.cpp" "${ADE_ROOT}/src/runtime/AgentToolHost.h"
    "${ADE_ROOT}/src/runtime/AttachmentFileBroker.cpp" "${ADE_ROOT}/src/runtime/AttachmentFileBroker.h"
    "${ADE_ROOT}/src/sandbox/SandboxHost.cpp" "${ADE_ROOT}/src/sandbox/SandboxHost.h")
target_include_directories(ade_probe_host PUBLIC "${ADE_ROOT}/src")
target_include_directories(ade_probe_host SYSTEM PRIVATE "${ADE_ROOT}/third_party/webview2/include")
target_link_libraries(ade_probe_host PUBLIC Qt6::Core Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Concurrent Qt6::ZlibPrivate crypt32 ole32 oleaut32 uuid shlwapi version)
add_executable(ade-runtime-app-tests "${ADE_ROOT}/tests/test_runtime_apps.cpp")
target_link_libraries(ade-runtime-app-tests PRIVATE ade_probe_host Qt6::Test)
add_custom_command(TARGET ade-runtime-app-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ADE_ROOT}/third_party/webview2/x64/WebView2Loader.dll" "$<TARGET_FILE_DIR:ade-runtime-app-tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ADE_ROOT}/audit/sandbox-probe-build/ADE-sandbox-worker.exe" "$<TARGET_FILE_DIR:ade-runtime-app-tests>")
add_executable(ade-app-core-tests "${ADE_ROOT}/tests/test_core.cpp")
target_link_libraries(ade-app-core-tests PRIVATE ade_probe_host Qt6::Test)
