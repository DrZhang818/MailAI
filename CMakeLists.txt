cmake_minimum_required(VERSION 3.20)

project(mail_ai_assistant LANGUAGES CXX)

# Core library and its public requirements.
add_library(mailai_core STATIC
    src/app/inbox.cpp
    src/config.cpp
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

# Executables consume the core library's public requirements.
foreach(mailai_target IN ITEMS
    mail_assistant
    inbox_test
    cli_test
    config_test
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

add_test(NAME cli_smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DMAIL_ASSISTANT=$<TARGET_FILE:mail_assistant>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake"
)
