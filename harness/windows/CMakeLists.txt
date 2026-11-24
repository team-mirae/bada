cmake_minimum_required(VERSION 3.21)

# The repository's root: one VERSION for everything in it, the engine and its header.
get_filename_component(BADA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(STRINGS "${BADA_ROOT}/VERSION" BADA_VERSION LIMIT_COUNT 1)
project(bada-tsf VERSION ${BADA_VERSION} LANGUAGES CXX)
string(REPLACE "." "," BADA_VERSION_NUMBERS "${BADA_VERSION},0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BADA_TESTS "Build the tests" ON)
option(BADA_WERROR "Warnings are errors" ON)
set(BADA_ENGINE_LIB "" CACHE FILEPATH "The engine's static library (bada.lib). Empty: cargo builds it here")
set(BADA_RUST_TARGET "" CACHE STRING "The Rust target of the engine. Empty: by the compiler's architecture")

if(MSVC)
  # The static CRT: the text service loads into every app and can't count on a VC++ runtime.
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
  add_compile_options(/W4 /utf-8 /permissive- /EHsc)
  if(BADA_WERROR)
    add_compile_options(/WX)
    add_link_options(/WX)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
  if(BADA_WERROR)
    add_compile_options(-Werror)
  endif()
endif()
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endif()
add_compile_definitions(BADA_VERSION="${PROJECT_VERSION}")

# The engine: the crate bada-engine, linked as a static library (the standard, §2). On Windows it
# is built with the static CRT, like the text service it goes into.
add_library(bada_engine STATIC IMPORTED GLOBAL)
if(NOT BADA_ENGINE_LIB)
  find_program(CARGO cargo HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin" REQUIRED)
  # cargo's output stays in this build folder: the repository's own target folder is left alone.
  set(BADA_CARGO_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
  if(WIN32)
    if(NOT BADA_RUST_TARGET)
      if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(BADA_RUST_TARGET i686-pc-windows-msvc)
      elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(BADA_RUST_TARGET aarch64-pc-windows-msvc)
      else()
        set(BADA_RUST_TARGET x86_64-pc-windows-msvc)
      endif()
    endif()
    set(BADA_ENGINE_LIB "${BADA_CARGO_DIR}/${BADA_RUST_TARGET}/release/bada.lib")
    set(BADA_CARGO_COMMAND ${CMAKE_COMMAND} -E env "RUSTFLAGS=-Ctarget-feature=+crt-static"
      ${CARGO} build --release -p bada-engine --target ${BADA_RUST_TARGET} --target-dir "${BADA_CARGO_DIR}")
  else()
    set(BADA_ENGINE_LIB "${BADA_CARGO_DIR}/release/libbada.a")
    set(BADA_CARGO_COMMAND ${CARGO} build --release -p bada-engine --target-dir "${BADA_CARGO_DIR}")
  endif()
  add_custom_target(bada_engine_build ALL
    COMMAND ${BADA_CARGO_COMMAND}
    WORKING_DIRECTORY "${BADA_ROOT}"
    BYPRODUCTS "${BADA_ENGINE_LIB}"
    COMMENT "cargo build --release -p bada-engine"
    VERBATIM)
  add_dependencies(bada_engine bada_engine_build)
endif()
set_target_properties(bada_engine PROPERTIES IMPORTED_LOCATION "${BADA_ENGINE_LIB}")
target_include_directories(bada_engine INTERFACE "${BADA_ROOT}/engine/include")
if(WIN32)
  # What Rust's standard library asks of Windows (cargo rustc -- --print native-static-libs).
  target_link_libraries(bada_engine INTERFACE kernel32 ntdll userenv ws2_32 dbghelp advapi32 bcrypt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(bada_engine INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# The core: no Windows headers. The key table, the engine wrapper, replies, the settings folder, the
# session and the context. It builds on any system, where its tests run too.
add_library(bada_core STATIC
  src/core/engine.cpp src/core/harness.cpp src/core/json.cpp src/core/keys.cpp
  src/core/keytable.cpp src/core/reply.cpp src/core/settings.cpp src/core/state_block.cpp)
target_include_directories(bada_core PUBLIC src/core)
target_link_libraries(bada_core PUBLIC bada_engine)

if(WIN32)
  # The text service: one DLL, the engine inside it.
  enable_language(RC)
  configure_file(src/tip/bada-tsf.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bada-tsf.rc" @ONLY)
  add_library(bada-tsf SHARED
    src/tip/candidate_window.cpp src/tip/display_attribute.cpp src/tip/dllmain.cpp
    src/tip/edit_session.cpp src/tip/lang_bar.cpp src/tip/log.cpp src/tip/shared_session.cpp
    src/tip/registration.cpp src/tip/state_shared.cpp src/tip/text_service.cpp
    src/tip/bada-tsf.def "${CMAKE_CURRENT_BINARY_DIR}/bada-tsf.rc")
  target_include_directories(bada-tsf PRIVATE src/tip)
  target_link_libraries(bada-tsf PRIVATE bada_core ole32 oleaut32 user32 gdi32 advapi32 shell32 uuid)
endif()

if(BADA_TESTS)
  enable_testing()

  # The standard's key-table cases, every one.
  add_executable(bada_keytable_cases tests/keytable_cases.cpp)
  target_link_libraries(bada_keytable_cases PRIVATE bada_core)
  add_test(NAME keytable_cases COMMAND bada_keytable_cases "${BADA_ROOT}/docs/cases/key-table.json")

  # The conformance transcripts, through the engine wrapper and the reply-applying code.
  add_executable(bada_transcript tests/transcript.cpp)
  target_link_libraries(bada_transcript PRIVATE bada_core)
  add_test(NAME transcripts COMMAND bada_transcript
    "${BADA_ROOT}/tools/conformance/transcripts/any-engine.jsonl"
    "${BADA_ROOT}/tools/conformance/transcripts/bada.jsonl")

  # The core, typed into the linked engine: settings, the key flow, `types`, Caps Lock, the state.
  add_executable(bada_core_tests tests/core_tests.cpp)
  target_link_libraries(bada_core_tests PRIVATE bada_core)
  target_compile_definitions(bada_core_tests PRIVATE BADA_ROOT="${BADA_ROOT}")
  add_test(NAME core COMMAND bada_core_tests)

  if(WIN32)
    # The state's shared memory, written and read as the text service and a script do.
    add_executable(bada_state_tests tests/state_tests.cpp src/tip/state_shared.cpp)
    target_include_directories(bada_state_tests PRIVATE src/tip)
    target_link_libraries(bada_state_tests PRIVATE bada_core advapi32 user32)
    add_test(NAME state COMMAND bada_state_tests)

    # The text service itself, in a TSF thread of the test's own with a text store of its own: keys
    # in through TSF's keystroke manager, text out of the store. It needs no desktop.
    add_executable(bada_tsf_host tests/tsf_host.cpp tests/text_store.cpp src/tip/state_shared.cpp)
    target_include_directories(bada_tsf_host PRIVATE src/tip)
    target_link_libraries(bada_tsf_host PRIVATE bada_core ole32 oleaut32 user32 advapi32 uuid)
    add_dependencies(bada_tsf_host bada-tsf)
    add_test(NAME tsf_host COMMAND bada_tsf_host --dll "$<TARGET_FILE:bada-tsf>")

    # The published state, as another program reads it.
    add_executable(bada_state tests/state_reader.cpp src/tip/state_shared.cpp)
    target_include_directories(bada_state PRIVATE src/tip)
    target_link_libraries(bada_state PRIVATE bada_core advapi32 user32)
  endif()
endif()
