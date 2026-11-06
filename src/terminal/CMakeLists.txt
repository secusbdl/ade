# Native static terminal engine. Kept separate so no C sources are accidentally
# compiled as C++, and no external package manager/runtime is required.
enable_language(C)
set(VTERM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libvterm-0.3.3)
add_library(ade_vterm STATIC
    ${VTERM_DIR}/src/encoding.c ${VTERM_DIR}/src/keyboard.c
    ${VTERM_DIR}/src/mouse.c ${VTERM_DIR}/src/parser.c
    ${VTERM_DIR}/src/pen.c ${VTERM_DIR}/src/screen.c
    ${VTERM_DIR}/src/state.c ${VTERM_DIR}/src/unicode.c
    ${VTERM_DIR}/src/vterm.c)
set_target_properties(ade_vterm PROPERTIES C_STANDARD 99 AUTOMOC OFF)
target_include_directories(ade_vterm PUBLIC ${VTERM_DIR}/include)
add_library(ade_terminal STATIC ConPtyProcess.cpp ConPtyProcess.h TerminalModel.cpp TerminalModel.h TerminalWidget.cpp TerminalWidget.h)
target_link_libraries(ade_terminal PUBLIC Qt6::Widgets PRIVATE ade_vterm)
target_include_directories(ade_terminal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(ade_terminal PRIVATE _WIN32_WINNT=0x0A00)
