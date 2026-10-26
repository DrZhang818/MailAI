cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED MAIL_ASSISTANT)
    message(FATAL_ERROR "MAIL_ASSISTANT must name the executable to test")
endif()

execute_process(
    COMMAND "${MAIL_ASSISTANT}" inbox
    RESULT_VARIABLE smoke_exit_code
    OUTPUT_VARIABLE smoke_stdout
    ERROR_VARIABLE smoke_stderr
    TIMEOUT 10
)

if(NOT "${smoke_exit_code}" STREQUAL "0")
    message(FATAL_ERROR
        "mail_assistant inbox failed (${smoke_exit_code}):\n${smoke_stderr}"
    )
endif()

if(NOT "${smoke_stdout}" MATCHES "课程通知")
    message(FATAL_ERROR "inbox output is missing 课程通知:\n${smoke_stdout}")
endif()

message(STATUS "CLI smoke passed")
