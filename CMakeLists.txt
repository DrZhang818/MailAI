cmake_minimum_required(VERSION 3.20)

project(mail_ai_assistant LANGUAGES CXX)

find_package(SQLite3 3.37 REQUIRED)

find_package(CURL 8.0 REQUIRED)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET "gmime-3.0>=3.2")

find_package(LibXml2 2.9 REQUIRED)

# Core library and its public requirements.
add_library(mailai_core STATIC
    src/app/inbox.cpp
    src/config.cpp
    src/store/sqlite_store.cpp
)

target_link_libraries(mailai_core PRIVATE
    SQLite::SQLite3
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

target_link_libraries(store_test PRIVATE SQLite::SQLite3)

# Executables consume the core library's public requirements.
foreach(mailai_target IN ITEMS
    mail_assistant
    inbox_test
    cli_test
    config_test
    store_test
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

add_test(NAME cli_smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DMAIL_ASSISTANT=$<TARGET_FILE:mail_assistant>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake"
)
