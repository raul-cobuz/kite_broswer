cmake_minimum_required(VERSION 3.16)
project(kite_browser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WEBKIT2 REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# Testable, UI-independent logic.
add_library(kite_core STATIC
    src/history.cpp
    src/session.cpp
    src/settings.cpp
    src/url_utils.cpp
)
target_include_directories(kite_core PUBLIC src)
target_link_libraries(kite_core PUBLIC PkgConfig::GLIB PkgConfig::SQLITE3)
target_compile_options(kite_core PRIVATE -Wall -Wextra)

set(KITE_UI_SOURCES
    src/adblock.cpp
    src/browser_window.cpp
    src/dark_mode.cpp
    src/downloads.cpp
    src/errors.cpp
    src/find_bar.cpp
    src/kite_pages.cpp
    src/permissions.cpp
    src/tabs.cpp
)

function(kite_add_browser target)
    add_executable(${target} src/main.cpp ${KITE_UI_SOURCES})
    target_link_libraries(${target} PRIVATE kite_core PkgConfig::GTK3 PkgConfig::WEBKIT2)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endfunction()

kite_add_browser(mybrowser)
kite_add_browser(mybrowser-debug)
set_target_properties(mybrowser-debug PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_compile_options(mybrowser-debug PRIVATE -g -O0 -UNDEBUG)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_url_utils tests/test_url_utils.cpp)
    target_link_libraries(test_url_utils PRIVATE kite_core)
    target_compile_options(test_url_utils PRIVATE -Wall -Wextra)
    add_test(NAME url_utils COMMAND test_url_utils)

    add_executable(test_settings tests/test_settings.cpp)
    target_link_libraries(test_settings PRIVATE kite_core)
    target_compile_options(test_settings PRIVATE -Wall -Wextra)
    add_test(NAME settings COMMAND test_settings)

    add_executable(test_history tests/test_history.cpp)
    target_link_libraries(test_history PRIVATE kite_core)
    target_compile_options(test_history PRIVATE -Wall -Wextra)
    add_test(NAME history COMMAND test_history)

    add_executable(test_session tests/test_session.cpp)
    target_link_libraries(test_session PRIVATE kite_core)
    target_compile_options(test_session PRIVATE -Wall -Wextra)
    add_test(NAME session COMMAND test_session)
endif()
