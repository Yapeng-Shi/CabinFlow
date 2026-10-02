execute_process(
    COMMAND "${RUNTIME_DEMO}"
    RESULT_VARIABLE demo_result
    OUTPUT_VARIABLE demo_output
    ERROR_VARIABLE demo_error
)

if(NOT demo_result EQUAL 0)
    message(FATAL_ERROR "runtime_demo failed: ${demo_result}\n${demo_error}\n${demo_output}")
endif()

if(NOT demo_output MATCHES "time_unix_ms=[0-9]+ event=")
    message(FATAL_ERROR "demo omitted structured event time\n${demo_output}")
endif()

function(expect_demo fragment)
    string(FIND "${demo_output}" "${fragment}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "missing demo event: ${fragment}\n${demo_output}")
    endif()
endfunction()

expect_demo("event=echo_completed node=echo_processor trace_id=demo-trace session_id=driver-session work_id=demo-work message_id=demo-message status=handled detail=echo:hello_cabinflow")
expect_demo("session_id=driver-concurrent-session work_id=driver-work message_id=driver-concurrent status=handled detail=echo:driver-text")
expect_demo("session_id=passenger-session work_id=passenger-work message_id=passenger-concurrent status=handled detail=echo:passenger-text")
expect_demo("session_id=driver-session work_id=demo-work message_id=demo-message status=duplicate_message")
expect_demo("session_id=driver-session work_id=demo-work message_id=after-final status=stream_finalized")
expect_demo("session_id=driver-session work_id=order-work message_id=order-low status=stale_sequence")
expect_demo("session_id=driver-session work_id=deadline-work message_id=expired-message status=expired")
expect_demo("session_id=driver-session work_id=cancel-work message_id=cancelled-message status=work_cancelled")
expect_demo("session_id=driver-session work_id=other-work message_id=other-work-message status=handled detail=echo:unaffected")
