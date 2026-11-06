add_library(ade_git STATIC
    GitTypes.h GitService.h GitService.cpp GitParsing.h GitParsing.cpp
    GitRepositoryWatcher.h GitRepositoryWatcher.cpp
    WorktreeUsage.h WorktreeUsage.cpp
    WorktreeUsageJournal.h WorktreeUsageJournal.cpp
    WorktreeUsageIdentity.h WorktreeUsageIdentity.cpp
    WorktreeCleanupController.h WorktreeCleanupController.cpp
    ProcessRunner.h ProcessRunner.cpp CodexSetup.cpp
    WorktreeSetupRunner.h WorktreeSetupRunner.cpp SetupScriptText.h SetupScriptText.cpp)
set_target_properties(ade_git PROPERTIES AUTOMOC ON)
target_compile_features(ade_git PUBLIC cxx_std_20)
target_include_directories(ade_git PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ade_git PUBLIC Qt6::Core)
