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

# 核心库及其公开依赖。
add_library(mailai_core STATIC
    src/app/inbox.cpp
    src/app/login_service.cpp
    src/app/services.cpp
    src/app/sync.cpp
    src/config/config.cpp
    src/config/credential_store.cpp
    src/config/login_profile.cpp
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

target_include_directories(mailai_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(mailai_core PUBLIC cxx_std_23)

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

add_executable(inbox_test
    tests/inbox_test.cpp
)

add_executable(cli_test
    tests/cli_test.cpp
    ${MAILAI_CLI_SOURCES}
)

add_executable(config_test
    tests/config_test.cpp
)

add_executable(login_profile_test tests/login_profile_test.cpp)
add_executable(login_service_test tests/login_service_test.cpp)
target_include_directories(login_service_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(login_service_test PRIVATE SQLite3::SQLite3)

if(APPLE)
    add_executable(credential_store_test tests/credential_store_test.cpp)
    target_include_directories(credential_store_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(credential_store_test PRIVATE mailai_core
        ${MAILAI_SECURITY_FRAMEWORK} ${MAILAI_COREFOUNDATION_FRAMEWORK})
    set_target_properties(credential_store_test PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(credential_store_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

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

add_executable(mime_test
    tests/mime_test.cpp
)

add_executable(imap_connection_test
    tests/imap_connection_test.cpp
)

add_executable(imap_snapshot_test
    tests/imap_snapshot_test.cpp
)

add_executable(imap_internal_date_test
    tests/imap_internal_date_test.cpp
)

add_executable(imap_fetch_protocol_test
    tests/imap_fetch_protocol_test.cpp
)

add_executable(sync_test
    tests/sync_test.cpp
)

# 真实邮箱探针仅在显式构建和启用时执行，不注册到默认 CTest。
add_executable(imap_live_probe EXCLUDE_FROM_ALL
    tests/imap_live_probe.cpp
)

target_link_libraries(imap_connection_test PRIVATE CURL::libcurl)
target_include_directories(imap_connection_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(imap_snapshot_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(imap_internal_date_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(imap_fetch_protocol_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sync_test PRIVATE SQLite3::SQLite3)
target_include_directories(sync_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(store_test PRIVATE SQLite3::SQLite3)
target_include_directories(store_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(store_list_test PRIVATE SQLite3::SQLite3)
target_link_libraries(store_find_test PRIVATE SQLite3::SQLite3)
target_link_libraries(store_sync_test PRIVATE SQLite3::SQLite3)
target_link_libraries(store_write_test PRIVATE SQLite3::SQLite3)
target_link_libraries(sqlite_context_test PRIVATE SQLite3::SQLite3)
target_include_directories(sqlite_context_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(data_dir_lock_test PRIVATE SQLite3::SQLite3 Threads::Threads)

# 可执行程序继承核心库的公开依赖。
foreach(mailai_target IN ITEMS
    mail_assistant
    inbox_test
    cli_test
    config_test
    login_profile_test
    login_service_test
    store_test
    store_list_test
    store_find_test
    store_sync_test
    store_write_test
    sqlite_context_test
    data_dir_lock_test
    mime_test
    imap_connection_test
    imap_snapshot_test
    imap_internal_date_test
    imap_fetch_protocol_test
    sync_test
    imap_live_probe
)
    target_link_libraries(${mailai_target} PRIVATE mailai_core)
endforeach()

foreach(mailai_target IN ITEMS mail_assistant cli_test)
    target_include_directories(${mailai_target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
endforeach()

# 项目各构建目标的公共配置。
foreach(mailai_target IN ITEMS
    mailai_core
    mail_assistant
    inbox_test
    cli_test
    config_test
    login_profile_test
    login_service_test
    store_test
    store_list_test
    store_find_test
    store_sync_test
    store_write_test
    sqlite_context_test
    data_dir_lock_test
    mime_test
    imap_connection_test
    imap_snapshot_test
    imap_internal_date_test
    imap_fetch_protocol_test
    sync_test
    imap_live_probe
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

# 注册 CTest 测试。
enable_testing()

add_test(NAME inbox COMMAND inbox_test)
add_test(NAME cli COMMAND cli_test)
add_test(NAME config COMMAND config_test)
add_test(NAME login_profile COMMAND login_profile_test)
add_test(NAME login_service COMMAND login_service_test)
if(APPLE)
    add_test(NAME credential_store COMMAND credential_store_test)
endif()
add_test(NAME store COMMAND store_test)
add_test(NAME store_list COMMAND store_list_test)
add_test(NAME store_find COMMAND store_find_test)
add_test(NAME store_sync COMMAND store_sync_test)
add_test(NAME store_write COMMAND store_write_test)
add_test(NAME sqlite_context COMMAND sqlite_context_test)
add_test(NAME data_dir_lock COMMAND data_dir_lock_test)
set_tests_properties(data_dir_lock PROPERTIES TIMEOUT 15)
add_test(NAME mime
    COMMAND mime_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mime"
)
add_test(NAME imap_fixture
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/imap_fixture_test.py"
)
set_tests_properties(imap_fixture PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 30
)
add_test(NAME imap_connection
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/imap_connection_test.py"
        "$<TARGET_FILE:imap_connection_test>"
)
set_tests_properties(imap_connection PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 60
)
add_test(NAME imap_snapshot COMMAND imap_snapshot_test)
add_test(NAME imap_internal_date COMMAND imap_internal_date_test)
add_test(NAME imap_fetch_protocol COMMAND imap_fetch_protocol_test)
add_test(NAME sync
    COMMAND sync_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mime"
)
add_test(NAME imap_fetch
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/imap_fetch_test.py"
        "$<TARGET_FILE:imap_connection_test>"
)
set_tests_properties(imap_fetch PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 60
)
add_test(NAME imap_retry
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/imap_retry_test.py"
        "$<TARGET_FILE:imap_connection_test>"
)
set_tests_properties(imap_retry PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 120
)

add_test(NAME cli_smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DMAIL_ASSISTANT=$<TARGET_FILE:mail_assistant>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake"
)
add_test(NAME cli_integration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_integration_test.py"
        "$<TARGET_FILE:mail_assistant>"
)
set_tests_properties(cli_integration PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 60
)
