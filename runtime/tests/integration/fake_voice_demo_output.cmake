if(NOT DEFINED FAKE_VOICE_DEMO OR NOT DEFINED FAKE_VOICE_TEST_DIR)
    message(FATAL_ERROR "fake voice test requires executable and output directory")
endif()

file(MAKE_DIRECTORY "${FAKE_VOICE_TEST_DIR}")
set(input "${FAKE_VOICE_TEST_DIR}/fixed_input.wav")
set(output_a "${FAKE_VOICE_TEST_DIR}/answer_a.wav")
set(output_b "${FAKE_VOICE_TEST_DIR}/answer_b.wav")

execute_process(
    COMMAND "${FAKE_VOICE_DEMO}" --write-fixture "${input}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "fixed WAV generation failed: ${error}")
endif()

execute_process(
    COMMAND "${FAKE_VOICE_DEMO}" "${input}" "${output_a}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE trace_a
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "first fake pipeline run failed: ${error}")
endif()
execute_process(
    COMMAND "${FAKE_VOICE_DEMO}" "${input}" "${output_b}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE trace_b
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "second fake pipeline run failed: ${error}")
endif()

file(SHA256 "${output_a}" digest_a)
file(SHA256 "${output_b}" digest_b)
if(NOT digest_a STREQUAL digest_b)
    message(FATAL_ERROR "fake TTS output is not byte-for-byte repeatable")
endif()
file(SIZE "${output_a}" answer_bytes)
file(READ "${output_a}" header OFFSET 0 LIMIT 12 HEX)
string(SUBSTRING "${header}" 0 8 riff)
string(SUBSTRING "${header}" 16 8 wave)
if(NOT answer_bytes EQUAL 12844 OR NOT riff STREQUAL "52494646" OR
   NOT wave STREQUAL "57415645")
    message(FATAL_ERROR "fake output is not the expected PCM WAV")
endif()

string(REGEX MATCH
    "stage=fake_asr trace=fake-voice-trace session=fake-session work=([^ ]+) "
    asr_line "${trace_a}")
if(NOT asr_line)
    message(FATAL_ERROR "ASR stage identity missing from trace")
endif()
set(work_id "${CMAKE_MATCH_1}")
foreach(stage IN ITEMS fake_asr router fake_llm fake_tts)
    string(FIND "${trace_a}"
        "stage=${stage} trace=fake-voice-trace session=fake-session work=${work_id} "
        position)
    if(position LESS 0)
        message(FATAL_ERROR "${stage} lost shared trace/session/work identity")
    endif()
endforeach()
if(NOT trace_a MATCHES
   "request_message_id=fake-audio-1 audio=fake_test_tone_not_speech")
    message(FATAL_ERROR "fake audio boundary is not labeled")
endif()
if(NOT trace_a MATCHES
   "state_after_exit=work_cancelled trace=fake-voice-trace session=fake-session")
    message(FATAL_ERROR "Exit did not close data admission for the work")
endif()

execute_process(
    COMMAND "${FAKE_VOICE_DEMO}"
        "${FAKE_VOICE_TEST_DIR}/missing.wav"
        "${FAKE_VOICE_TEST_DIR}/unused.wav"
    RESULT_VARIABLE result
    ERROR_VARIABLE error
)
if(result EQUAL 0 OR NOT error MATCHES "cannot open input WAV")
    message(FATAL_ERROR "missing input must fail explicitly")
endif()

file(WRITE "${FAKE_VOICE_TEST_DIR}/invalid.wav" "not a WAV")
execute_process(
    COMMAND "${FAKE_VOICE_DEMO}"
        "${FAKE_VOICE_TEST_DIR}/invalid.wav"
        "${FAKE_VOICE_TEST_DIR}/unused.wav"
    RESULT_VARIABLE result
    ERROR_VARIABLE error
)
if(result EQUAL 0 OR NOT error MATCHES "invalid_payload")
    message(FATAL_ERROR "invalid WAV must fail at fake ASR payload validation")
endif()
