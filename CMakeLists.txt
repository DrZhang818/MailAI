cmake_minimum_required(VERSION 3.20)

project(mail_ai_assistant LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

find_package(SQLite3 3.37 REQUIRED)

if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

find_package(CURL 8.17.0 REQUIRED)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET "gmime-3.0>=3.2")

find_package(LibXml2 2.9 REQUIRED)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

# 所有项目目标共用的语言标准和告警选项。
add_library(mailai_build_options INTERFACE)
target_compile_options(mailai_build_options INTERFACE -Wall -Wextra -Wpedantic)

function(mailai_configure_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_link_libraries(${target} PRIVATE mailai_build_options)
endfunction()

# 核心库及其公开依赖。
add_library(mailai_core STATIC
    src/app/inbox.cpp
    src/app/login_service.cpp
    src/app/services.cpp
    src/app/sync.cpp
    src/config/config.cpp
    src/config/credential_store.cpp
    src/config/login_profile.cpp
    src/mail/html_extract.cpp
    src/mail/imap.cpp
    src/mail/imap_protocol.cpp
    src/mail/mime.cpp
    src/store/data_dir_lock.cpp
    src/store/sqlite_store.cpp
    src/store/sqlite_sync.cpp
    src/store/sqlite_write.cpp
    src/store/sqlite_utils.cpp
    src/store/sqlite_rows.cpp
    src/store/schema.cpp
    src/util/secure_fs.cpp
)

target_link_libraries(mailai_core PRIVATE
    Threads::Threads
    SQLite3::SQLite3
    CURL::libcurl
    PkgConfig::GMIME
    LibXml2::LibXml2
)

if(APPLE)
    find_library(MAILAI_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(MAILAI_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(mailai_core PRIVATE
        ${MAILAI_SECURITY_FRAMEWORK}
        ${MAILAI_COREFOUNDATION_FRAMEWORK}
    )
endif()

target_include_directories(mailai_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(mailai_core PUBLIC cxx_std_23)
mailai_configure_target(mailai_core)

# 可执行程序与 CLI 测试共用同一组命令行实现。
set(MAILAI_CLI_SOURCES
    src/cli/cli.cpp
    src/cli/cli_login.cpp
    src/cli/cli_output.cpp
    src/cli/cli_terminal.cpp
)

add_executable(mail_assistant
    src/main.cpp
    ${MAILAI_CLI_SOURCES}
)
target_link_libraries(mail_assistant PRIVATE mailai_core)
target_include_directories(mail_assistant PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
mailai_configure_target(mail_assistant)

# 测试可执行程序：链接核心库，INTERNAL 表示需要访问 src 下的内部头文件。
function(mailai_add_test_executable target)
    cmake_parse_arguments(ARG "INTERNAL;EXCLUDE_FROM_ALL" "" "SOURCES;LIBRARIES" ${ARGN})
    if(ARG_EXCLUDE_FROM_ALL)
        add_executable(${target} EXCLUDE_FROM_ALL ${ARG_SOURCES})
    else()
        add_executable(${target} ${ARG_SOURCES})
    endif()
    target_link_libraries(${target} PRIVATE mailai_core ${ARG_LIBRARIES})
    if(ARG_INTERNAL)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
    mailai_configure_target(${target})
endfunction()

# C++ 单元测试：创建 <name>_test 程序并注册名为 <name> 的 CTest。
function(mailai_add_unit_test name)
    cmake_parse_arguments(ARG "INTERNAL" "WORKING_DIRECTORY;TIMEOUT" "SOURCES;LIBRARIES" ${ARGN})
    set(internal_flag)
    if(ARG_INTERNAL)
        set(internal_flag INTERNAL)
    endif()
    mailai_add_test_executable(${name}_test ${internal_flag}
        SOURCES ${ARG_SOURCES}
        LIBRARIES ${ARG_LIBRARIES}
    )
    if(ARG_WORKING_DIRECTORY)
        add_test(NAME ${name} COMMAND ${name}_test WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}")
    else()
        add_test(NAME ${name} COMMAND ${name}_test)
    endif()
    if(ARG_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
    endif()
endfunction()

# Python 驱动的集成测试：脚本位于 tests 目录，其余参数传给脚本。
function(mailai_add_python_test name script timeout)
    add_test(NAME ${name}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/${script}" ${ARGN}
    )
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT ${timeout}
    )
endfunction()

set(MAILAI_MIME_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mime")

# 注册 CTest 测试。
enable_testing()

mailai_add_unit_test(inbox SOURCES tests/inbox_test.cpp)
mailai_add_unit_test(cli INTERNAL SOURCES tests/cli_test.cpp ${MAILAI_CLI_SOURCES})
mailai_add_unit_test(config SOURCES tests/config_test.cpp)
mailai_add_unit_test(login_profile SOURCES tests/login_profile_test.cpp)
mailai_add_unit_test(login_service INTERNAL
    SOURCES tests/login_service_test.cpp
    LIBRARIES SQLite3::SQLite3
)
if(APPLE)
    mailai_add_unit_test(credential_store INTERNAL
        SOURCES tests/credential_store_test.cpp
        LIBRARIES ${MAILAI_SECURITY_FRAMEWORK} ${MAILAI_COREFOUNDATION_FRAMEWORK}
    )
endif()
mailai_add_unit_test(store INTERNAL SOURCES tests/store_test.cpp LIBRARIES SQLite3::SQLite3)
mailai_add_unit_test(store_list SOURCES tests/store_list_test.cpp LIBRARIES SQLite3::SQLite3)
mailai_add_unit_test(store_find SOURCES tests/store_find_test.cpp LIBRARIES SQLite3::SQLite3)
mailai_add_unit_test(store_sync SOURCES tests/store_sync_test.cpp LIBRARIES SQLite3::SQLite3)
mailai_add_unit_test(store_write SOURCES tests/store_write_test.cpp LIBRARIES SQLite3::SQLite3)
mailai_add_unit_test(sqlite_context INTERNAL
    SOURCES tests/sqlite_context_test.cpp
    LIBRARIES SQLite3::SQLite3
)
mailai_add_unit_test(data_dir_lock
    SOURCES tests/data_dir_lock_test.cpp
    LIBRARIES SQLite3::SQLite3 Threads::Threads
    TIMEOUT 15
)
mailai_add_unit_test(mime SOURCES tests/mime_test.cpp WORKING_DIRECTORY "${MAILAI_MIME_FIXTURES}")
mailai_add_unit_test(imap_snapshot INTERNAL SOURCES tests/imap_snapshot_test.cpp)
mailai_add_unit_test(imap_internal_date INTERNAL SOURCES tests/imap_internal_date_test.cpp)
mailai_add_unit_test(imap_fetch_protocol INTERNAL SOURCES tests/imap_fetch_protocol_test.cpp)
mailai_add_unit_test(sync INTERNAL
    SOURCES tests/sync_test.cpp
    LIBRARIES SQLite3::SQLite3
    WORKING_DIRECTORY "${MAILAI_MIME_FIXTURES}"
)

# imap_connection_test 由多个 Python 测试驱动，本身不直接注册。
mailai_add_test_executable(imap_connection_test INTERNAL
    SOURCES tests/imap_connection_test.cpp
    LIBRARIES CURL::libcurl
)

# 真实邮箱探针仅在显式构建和启用时执行，不注册到默认 CTest。
mailai_add_test_executable(imap_live_probe EXCLUDE_FROM_ALL SOURCES tests/imap_live_probe.cpp)

mailai_add_python_test(imap_fixture imap_fixture_test.py 30)
mailai_add_python_test(imap_connection imap_connection_test.py 60
    "$<TARGET_FILE:imap_connection_test>"
)
mailai_add_python_test(imap_fetch imap_fetch_test.py 60 "$<TARGET_FILE:imap_connection_test>")
mailai_add_python_test(imap_retry imap_retry_test.py 120 "$<TARGET_FILE:imap_connection_test>")

add_test(NAME cli_smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DMAIL_ASSISTANT=$<TARGET_FILE:mail_assistant>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake"
)
mailai_add_python_test(cli_integration cli_integration_test.py 60 "$<TARGET_FILE:mail_assistant>")
