cmake_minimum_required(VERSION 3.20)

project(mail_ai_assistant LANGUAGES CXX)

find_package(Threads REQUIRED)

find_package(SQLite3 3.37 REQUIRED)

if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

find_package(CURL 8.0 REQUIRED)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET "gmime-3.0>=3.2")

find_package(LibXml2 2.9 REQUIRED)

# Core library and its public requirements.
add_library(mailai_core STATIC
    src/app/inbox.cpp
    src/config.cpp
    src/store/data_dir_lock.cpp
    src/store/sqlite_store.cpp
    src/store/sqlite_sync.cpp
    src/store/sqlite_write.cpp
    src/store/sqlite_utils.cpp
    src/store/schema.cpp
)

target_link_libraries(mailai_core PRIVATE
    Threads::Threads
    SQLite3::SQLite3
    CURL::libcurl
    PkgConfig::GMIME
    LibXml2::LibXml2
)

target_include_directories(mailai_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(mailai_core PUBLIC cxx_std_23)

# Application and test executables.
add_executable(mail_assistant
    src/main.cpp
    src/cli.cpp
)

add_executable(inbox_test
    tests/inbox_test.cpp
)

add_executable(cli_test
    tests/cli_test.cpp
    src/cli.cpp
)

add_executable(config_test
    tests/config_test.cpp
)

add_executable(store_test
    tests/store_test.cpp
)

add_executable(store_list_test
    tests/store_list_test.cpp
)

add_executable(store_find_test
    tests/store_find_test.cpp
)

add_executable(store_sync_test
    tests/store_sync_test.cpp
)

add_executable(store_write_test
    tests/store_write_test.cpp
)

add_executable(sqlite_context_test
    tests/sqlite_context_test.cpp
)

add_executable(data_dir_lock_test
    tests/data_dir_lock_test.cpp
)

target_link_libraries(store_test PRIVATE SQLite3::SQLite3)
target_include_directories(store_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(store_list_test PRIVATE SQLite3::SQLite3)
target_link_libraries(store_find_test PRIVATE SQLite3::SQLite3)
target_link_libraries(store_sync_test PRIVATE SQLite3::SQLite3)
target_link_libraries(store_write_test PRIVATE SQLite3::SQLite3)
target_link_libraries(sqlite_context_test PRIVATE SQLite3::SQLite3)
target_include_directories(sqlite_context_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(data_dir_lock_test PRIVATE SQLite3::SQLite3 Threads::Threads)

# Executables consume the core library's public requirements.
foreach(mailai_target IN ITEMS
    mail_assistant
    inbox_test
    cli_test
    config_test
    store_test
    store_list_test
    store_find_test
    store_sync_test
    store_write_test
    sqlite_context_test
    data_dir_lock_test
)
    target_link_libraries(${mailai_target} PRIVATE mailai_core)
endforeach()

foreach(mailai_target IN ITEMS mail_assistant cli_test)
    target_include_directories(${mailai_target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
endforeach()

# Shared settings for this project's targets.
foreach(mailai_target IN ITEMS
    mailai_core
    mail_assistant
    inbox_test
    cli_test
    config_test
    store_test
    store_list_test
    store_find_test
    store_sync_test
    store_write_test
    sqlite_context_test
    data_dir_lock_test
)
    set_target_properties(${mailai_target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_compile_options(${mailai_target} PRIVATE
        -Wall -Wextra -Wpedantic
    )
endforeach()

# CTest registration.
enable_testing()

add_test(NAME inbox COMMAND inbox_test)
add_test(NAME cli COMMAND cli_test)
add_test(NAME config COMMAND config_test)
add_test(NAME store COMMAND store_test)
add_test(NAME store_list COMMAND store_list_test)
add_test(NAME store_find COMMAND store_find_test)
add_test(NAME store_sync COMMAND store_sync_test)
add_test(NAME store_write COMMAND store_write_test)
add_test(NAME sqlite_context COMMAND sqlite_context_test)
add_test(NAME data_dir_lock COMMAND data_dir_lock_test)
set_tests_properties(data_dir_lock PROPERTIES TIMEOUT 15)

add_test(NAME cli_smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DMAIL_ASSISTANT=$<TARGET_FILE:mail_assistant>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake"
)
