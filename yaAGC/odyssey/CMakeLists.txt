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

add_executable(odyssey-embedded-demo-test
  embedded_demo_test.c
  ${PROJECT_SOURCE_DIR}/yaAGC/agc_engine.c)

target_include_directories(odyssey-embedded-demo-test
  PRIVATE ${PROJECT_SOURCE_DIR}/yaAGC)

if(APPLE OR WIN32)
  target_compile_definitions(odyssey-embedded-demo-test PRIVATE STDC_HEADERS)
endif()

if(NOT MSVC)
  target_link_libraries(odyssey-embedded-demo-test PRIVATE m)
endif()

add_test(NAME odyssey-embedded-demo COMMAND odyssey-embedded-demo-test)
set_tests_properties(odyssey-embedded-demo PROPERTIES TIMEOUT 15)

add_executable(odyssey-single-host-test
  odyssey_single_host_test.c
  odyssey_engine.c
  ${PROJECT_SOURCE_DIR}/yaAGC/agc_engine.c
  ${PROJECT_SOURCE_DIR}/yaAGC/agc_engine_init.c
  ${PROJECT_SOURCE_DIR}/yaAGC/rfopen.c)

target_include_directories(odyssey-single-host-test
  PRIVATE ${PROJECT_SOURCE_DIR}/yaAGC ${CMAKE_CURRENT_SOURCE_DIR})

if(APPLE OR WIN32)
  target_compile_definitions(odyssey-single-host-test PRIVATE STDC_HEADERS)
endif()

if(NOT MSVC)
  target_link_libraries(odyssey-single-host-test PRIVATE m)
endif()

add_test(NAME odyssey-single-host COMMAND odyssey-single-host-test)
set_tests_properties(odyssey-single-host PROPERTIES TIMEOUT 15)
