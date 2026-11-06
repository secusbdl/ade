# Optional reference-only experiments. The root project supplies the static
# toolchain and Qt packages and enables this directory only on explicit opt-in.
# Neither library is linked into ADE or changes production tool definitions.
if(NOT WIN32)
    message(FATAL_ERROR "The native reference experiments currently require Windows.")
endif()

add_library(ade_experimental_file_mutations STATIC
    file-mutations/src/FileMutationStore.cpp
    file-mutations/src/FileMutationStore.h
)
target_include_directories(ade_experimental_file_mutations PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/file-mutations/src"
)
target_link_libraries(ade_experimental_file_mutations PUBLIC Qt6::Core Qt6::Sql)

add_library(ade_experimental_git_usage STATIC
    git-usage/src/git/WorktreeUsage.cpp
    git-usage/src/git/WorktreeUsage.h
    git-usage/src/git/WorktreeUsageJournal.cpp
    git-usage/src/git/WorktreeUsageJournal.h
    git-usage/src/git/WorktreeUsageIdentity.cpp
    git-usage/src/git/WorktreeUsageIdentity.h
    git-usage/src/git/GitTypes.h
)
target_include_directories(ade_experimental_git_usage PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/git-usage/src"
)
target_link_libraries(ade_experimental_git_usage PUBLIC Qt6::Core)

qt_add_executable(test_reference_file_mutations
    file-mutations/tests/test_file_mutations.cpp
)
target_link_libraries(test_reference_file_mutations PRIVATE
    ade_experimental_file_mutations Qt6::Test
)

qt_add_executable(test_reference_git_usage
    git-usage/tests/test_git_usage.cpp
)
target_link_libraries(test_reference_git_usage PRIVATE
    ade_experimental_git_usage Qt6::Test
)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Testing")
add_test(NAME reference_file_mutations
    COMMAND test_reference_file_mutations
    -o "${CMAKE_BINARY_DIR}/Testing/reference_file_mutations.txt,txt"
)
add_test(NAME reference_git_usage
    COMMAND test_reference_git_usage
    -o "${CMAKE_BINARY_DIR}/Testing/reference_git_usage.txt,txt"
)
set_tests_properties(reference_file_mutations reference_git_usage PROPERTIES
    TIMEOUT 90
    LABELS "reference;experimental"
)
