add_executable(odyssey-embedded-baseline-test
  embedded_baseline_test.c
  ${PROJECT_SOURCE_DIR}/yaAGC/agc_engine.c
  ${PROJECT_SOURCE_DIR}/yaAGC/agc_engine_init.c
  ${PROJECT_SOURCE_DIR}/yaAGC/rfopen.c)

target_include_directories(odyssey-embedded-baseline-test
  PRIVATE ${PROJECT_SOURCE_DIR}/yaAGC)

if(APPLE OR WIN32)
  target_compile_definitions(odyssey-embedded-baseline-test PRIVATE STDC_HEADERS)
endif()

if(NOT MSVC)
  target_link_libraries(odyssey-embedded-baseline-test PRIVATE m)
endif()

add_test(NAME odyssey-embedded-baseline COMMAND odyssey-embedded-baseline-test)
set_tests_properties(odyssey-embedded-baseline PROPERTIES TIMEOUT 15)
