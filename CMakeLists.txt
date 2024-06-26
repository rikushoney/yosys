cmake_minimum_required(VERSION 3.10)

if(WIN32)
  set(
    CMAKE_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE
    STRING
    "Vcpkg toolchain file"
  )
endif()

project(yosys)

include(${PROJECT_SOURCE_DIR}/cmake/CPM.cmake)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules/)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(YOSYS_LINK_ABC "Whether to statically link to abc" OFF)

add_library(yosys SHARED)
target_include_directories(yosys PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(yosys PUBLIC _YOSYS_)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(yosys PUBLIC -Wno-deprecated-declarations)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # target_compile_options(yosys PUBLIC)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(yosys PUBLIC /wd4244)
  target_compile_definitions(yosys
    PUBLIC UNICODE _CRT_SECURE_NO_WARNINGS
    PRIVATE YOSYS_DLL_EXPORT
  )
  set_target_properties(yosys PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(WIN32)
  set(WFB_BUILD_DIR ${CMAKE_BINARY_DIR}/winflexbison)
  CPMAddPackage(winflexbison
    GIT_REPOSITORY "https://github.com/lexxmark/winflexbison.git"
    GIT_TAG "300f48b"
    DOWNLOAD_ONLY
  )
  execute_process(
    COMMAND ${CMAKE_COMMAND}
            -S ${winflexbison_SOURCE_DIR}
            -G ${CMAKE_GENERATOR}
            -DCMAKE_BUILD_TYPE=Release
            -B ${WFB_BUILD_DIR}
  )
  execute_process(COMMAND ${CMAKE_COMMAND} --build ${WFB_BUILD_DIR} --config Release)
  find_program(BISON_EXECUTABLE
    NAMES win_bison
    HINTS ${winflexbison_SOURCE_DIR}/bin/Release
    DOC "path to the bison executable"
    REQUIRED
  )
  find_program(FLEX_EXECUTABLE
    NAMES win_flex
    HINTS ${winflexbison_SOURCE_DIR}/bin/Release
    DOC "path to the flex executable"
    REQUIRED
  )
endif()

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

bison_target(rtlil_parser
  ${CMAKE_SOURCE_DIR}/frontends/rtlil/rtlil_parser.y
  ${CMAKE_BINARY_DIR}/frontends/rtlil/rtlil_parser.tab.cc
  DEFINES_FILE ${CMAKE_BINARY_DIR}/frontends/rtlil/rtlil_parser.tab.hh
)

if(NOT DEFINED BISON_rtlil_parser_DEFINED)
  message(FATAL_ERROR "failed to generate rtlil parser")
endif()

flex_target(rtlil_lexer
  ${CMAKE_SOURCE_DIR}/frontends/rtlil/rtlil_lexer.l
  ${CMAKE_BINARY_DIR}/frontends/rtlil/rtlil_lexer.cc
  DEFINES_FILE ${CMAKE_BINARY_DIR}/frontends/rtlil/rtlil_lexer.hh
)

if(NOT DEFINED FLEX_rtlil_lexer_DEFINED)
  message(FATAL_ERROR "failed to generate rtlil lexer")
endif()

add_flex_bison_dependency(rtlil_lexer rtlil_parser)

bison_target(verilog_parser
  ${CMAKE_SOURCE_DIR}/frontends/verilog/verilog_parser.y
  ${CMAKE_BINARY_DIR}/frontends/verilog/verilog_parser.tab.cc
  DEFINES_FILE ${CMAKE_BINARY_DIR}/frontends/verilog/verilog_parser.tab.hh
)

if(NOT DEFINED BISON_verilog_parser_DEFINED)
  message(FATAL_ERROR "failed to generate verilog parser")
endif()

flex_target(verilog_lexer
  ${CMAKE_SOURCE_DIR}/frontends/verilog/verilog_lexer.l
  ${CMAKE_BINARY_DIR}/frontends/verilog/verilog_lexer.cc
  DEFINES_FILE ${CMAKE_BINARY_DIR}/frontends/verilog/verilog_lexer.hh
)

if(NOT DEFINED FLEX_verilog_lexer_DEFINED)
  message(FATAL_ERROR "failed to generate verilog lexer")
endif()

add_flex_bison_dependency(verilog_lexer verilog_parser)

target_include_directories(yosys PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_sources(yosys
  PRIVATE
  ${BISON_rtlil_parser_OUTPUT_SOURCE}
  ${FLEX_rtlil_lexer_OUTPUTS}
  ${BISON_verilog_parser_OUTPUT_SOURCE}
  ${FLEX_verilog_lexer_OUTPUTS}
)

if(WIN32)
  find_package(unofficial-readline-win32 CONFIG REQUIRED)
  target_link_libraries(yosys PUBLIC unofficial::readline-win32::readline)
else()
  find_package(Readline REQUIRED)
  target_link_libraries(yosys PUBLIC Readline::Readline)
endif()
target_compile_definitions(yosys PUBLIC YOSYS_ENABLE_READLINE)

if(NOT WIN32)
  find_package(TCL)
  if(NOT TCL_FOUND)
    message(FATAL_ERROR "could NOT find TCL")
  endif()
  target_link_libraries(yosys PUBLIC ${TCL_LIBRARY})
  target_include_directories(yosys PUBLIC ${TCL_INCLUDE_PATH})
  target_compile_definitions(yosys PUBLIC YOSYS_ENABLE_TCL)
endif()

if(NOT YOSYS_LINK_ABC AND NOT WIN32)
  find_package(ABC)
  target_compile_definitions(yosys PUBLIC ABCEXTERNAL="${ABC_EXECUTABLE}")
endif()
if(YOSYS_LINK_ABC OR WIN32 OR NOT ABC_FOUND)
  CPMAddPackage(
    NAME abc
    OPTIONS "ABC_NAMESPACE abc"
    GIT_REPOSITORY "https://github.com/rikushoney/abc_cmake.git"
    GIT_TAG "64258d84"
  )
  if(NOT WIN32)
    target_compile_options(abc PRIVATE -fPIC)
  endif()
  target_link_libraries(yosys PRIVATE abc)
  target_compile_definitions(yosys PUBLIC YOSYS_LINK_ABC)
endif()
target_compile_definitions(yosys PUBLIC YOSYS_ENABLE_ABC)

find_package(ZLIB REQUIRED)
target_link_libraries(yosys PUBLIC ZLIB::ZLIB)
target_compile_definitions(yosys PUBLIC YOSYS_ENABLE_ZLIB)

add_subdirectory(backends)
add_subdirectory(frontends)
add_subdirectory(kernel)
add_subdirectory(libs)
add_subdirectory(passes)
add_subdirectory(techlibs)

add_executable(yosys_exe)
set_property(TARGET yosys_exe PROPERTY OUTPUT_NAME yosys)
target_sources(yosys_exe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel/driver.cc)
target_link_libraries(yosys_exe PRIVATE yosys)

target_sources(yosys
  PUBLIC
  FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
  FILES
  kernel/binding.h
  kernel/cellaigs.h
  kernel/celledges.h
  kernel/celltypes.h
  kernel/consteval.h
  kernel/constids.inc
  kernel/cost.h
  kernel/ff.h
  kernel/ffinit.h
  kernel/ffmerge.h
  kernel/fmt.h
  kernel/fstdata.h
  kernel/hashlib.h
  kernel/json.h
  kernel/log.h
  kernel/macc.h
  kernel/modtools.h
  kernel/mem.h
  kernel/qcsat.h
  kernel/register.h
  kernel/rtlil.h
  kernel/satgen.h
  kernel/sigtools.h
  kernel/timinginfo.h
  kernel/utils.h
  kernel/yosys.h
  kernel/yw.h
  libs/ezsat/ezsat.h
  libs/ezsat/ezminisat.h
  libs/fst/fstapi.h
  libs/sha1/sha1.h
  libs/json11/json11.hpp
  passes/fsm/fsmdata.h
  frontends/ast/ast.h
  frontends/ast/ast_binding.h
  frontends/blif/blifparse.h
  backends/rtlil/rtlil_backend.h
  backends/cxxrtl/cxxrtl.h
  backends/cxxrtl/cxxrtl_vcd.h
  backends/cxxrtl/cxxrtl_capi.cc
  backends/cxxrtl/cxxrtl_capi.h
  backends/cxxrtl/cxxrtl_vcd_capi.cc
  backends/cxxrtl/cxxrtl_vcd_capi.h
)

install(TARGETS yosys yosys_exe FILE_SET HEADERS)
