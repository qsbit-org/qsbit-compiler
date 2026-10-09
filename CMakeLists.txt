cmake_minimum_required(VERSION 3.24)
project(qsbit_compiler VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 21)
  message(FATAL_ERROR "qsbit-compiler requires LLVM 21")
endif()
find_program(QSBIT_LLD NAMES ld.lld-21 ld.lld HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
execute_process(COMMAND "${QSBIT_LLD}" --version OUTPUT_VARIABLE LLD_VERSION)
if(NOT LLD_VERSION MATCHES "LLD 21\\.")
  message(FATAL_ERROR "qsbit-compiler requires LLD 21: ${QSBIT_LLD}")
endif()
add_library(qsbit_compiler
  lib/QIR/Reader.cpp lib/QIR/Adaptive.cpp lib/Target/Target.cpp
  lib/Scheduling/Schedule.cpp lib/Scheduling/Adaptive.cpp lib/Target/Mapping.cpp
  lib/CodeGen/Emitter.cpp lib/CodeGen/Adaptive.cpp)
target_include_directories(qsbit_compiler PUBLIC include)
target_include_directories(qsbit_compiler SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(qsbit_compiler PRIVATE ${LLVM_DEFINITIONS_LIST})
target_link_libraries(qsbit_compiler PUBLIC LLVM)
add_executable(qsbitc tools/qsbitc/main.cpp)
target_link_libraries(qsbitc PRIVATE qsbit_compiler)
set_target_properties(qsbitc PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
target_compile_definitions(qsbitc PRIVATE QSBIT_LLD="${QSBIT_LLD}")
foreach(t qsbit_compiler qsbitc)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME compiler.install COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/install_test.py" "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}" "${CMAKE_INSTALL_BINDIR}" "${CMAKE_INSTALL_DATADIR}")
  set_tests_properties(compiler.install PROPERTIES TIMEOUT 60)
  add_test(NAME compiler.contracts COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/compiler_test.py" "$<TARGET_FILE:qsbitc>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(compiler.contracts PROPERTIES TIMEOUT 60)
  add_test(NAME compiler.adaptive COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/adaptive_test.py" "$<TARGET_FILE:qsbitc>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(compiler.adaptive PROPERTIES TIMEOUT 60)
  set(QSBIT_SIM_EXECUTABLE "" CACHE FILEPATH "Optional qsbit-sim executable for integration tests")
  option(QSBIT_TEST_AER "Run numerical Bell integration tests (requires simulator Aer backend)" OFF)
  set(QSBIT_BLOQ_EXPORTER "" CACHE FILEPATH "Optional built bloq_qir export example for end-to-end tests")
  option(QSBIT_TEST_QEC "Run decoder feedback tests (requires simulator QEC extra)" OFF)
  if(QSBIT_SIM_EXECUTABLE)
    if(QSBIT_TEST_QEC)
      add_test(NAME integration.qec COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/qec_test.py" "$<TARGET_FILE:qsbitc>"
        "${PROJECT_SOURCE_DIR}" "${QSBIT_SIM_EXECUTABLE}")
      set_tests_properties(integration.qec PROPERTIES TIMEOUT 180)
      if(QSBIT_BLOQ_EXPORTER AND QSBIT_TEST_AER)
        add_test(NAME integration.bloq COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/bloq_integration_test.py" "$<TARGET_FILE:qsbitc>"
          "${PROJECT_SOURCE_DIR}" "${QSBIT_SIM_EXECUTABLE}" "${QSBIT_BLOQ_EXPORTER}")
        set_tests_properties(integration.bloq PROPERTIES TIMEOUT 600)
      endif()
    endif()
    add_test(NAME integration.mock COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/integration_test.py" "$<TARGET_FILE:qsbitc>"
      "${PROJECT_SOURCE_DIR}" "${QSBIT_SIM_EXECUTABLE}" mock)
    set_tests_properties(integration.mock PROPERTIES TIMEOUT 60)
    if(QSBIT_TEST_AER)
      add_test(NAME integration.aer COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/integration_test.py" "$<TARGET_FILE:qsbitc>"
        "${PROJECT_SOURCE_DIR}" "${QSBIT_SIM_EXECUTABLE}" aer)
      set_tests_properties(integration.aer PROPERTIES TIMEOUT 180)
    endif()
  endif()
endif()
install(TARGETS qsbitc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY targets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/qsbit-compiler/targets)
