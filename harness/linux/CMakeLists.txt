cmake_minimum_required(VERSION 3.21)

# The repository's root: one VERSION for everything in it, the engine and its header.
get_filename_component(BADA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(STRINGS "${BADA_ROOT}/VERSION" BADA_VERSION LIMIT_COUNT 1)
project(bada-linux VERSION ${BADA_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

option(BADA_FCITX5 "Build the Fcitx5 addon" ON)
option(BADA_IBUS "Build the IBus engine" ON)
option(BADA_TESTS "Build the tests" ON)
option(BADA_CLI "Build and install the command line and the settings page" ON)
option(BADA_WERROR "Warnings are errors" OFF)
set(BADA_ENGINE_LIB "" CACHE FILEPATH "The engine's static library (libbada.a). Empty: cargo builds it here")

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

add_compile_options(-Wall -Wextra)
if(BADA_WERROR)
  add_compile_options(-Werror)
endif()
add_compile_definitions(BADA_VERSION="${PROJECT_VERSION}")

# The engine: the crate bada-engine, linked as a static library (the standard, §2).
add_library(bada_engine STATIC IMPORTED GLOBAL)
if(NOT BADA_ENGINE_LIB)
  find_program(CARGO cargo REQUIRED)
  # Where cargo puts it: the workspace's target folder, unless CARGO_TARGET_DIR says otherwise.
  if(DEFINED ENV{CARGO_TARGET_DIR})
    set(BADA_ENGINE_LIB "$ENV{CARGO_TARGET_DIR}/release/libbada.a")
  else()
    set(BADA_ENGINE_LIB "${BADA_ROOT}/target/release/libbada.a")
  endif()
  add_custom_target(bada_engine_build ALL
    COMMAND ${CARGO} build --release -p bada-engine
    WORKING_DIRECTORY "${BADA_ROOT}"
    BYPRODUCTS "${BADA_ENGINE_LIB}"
    COMMENT "cargo build --release -p bada-engine"
    VERBATIM)
  add_dependencies(bada_engine bada_engine_build)
endif()
set_target_properties(bada_engine PROPERTIES IMPORTED_LOCATION "${BADA_ENGINE_LIB}")
target_include_directories(bada_engine INTERFACE "${BADA_ROOT}/engine/include")
target_link_libraries(bada_engine INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)

# The core: no framework headers. The key table, the engine wrapper, replies, the settings folder,
# and the session and context every frontend shares.
add_library(bada_core STATIC
  src/core/engine.cpp src/core/harness.cpp src/core/json.cpp src/core/keys.cpp
  src/core/keytable.cpp src/core/reply.cpp src/core/settings.cpp)
target_include_directories(bada_core PUBLIC src/core)
target_link_libraries(bada_core PUBLIC bada_engine)

# The engine's data (the standard, §11): each file is the buffer of its name. With their licenses.
set(BADA_DATA_NAMES ko-words.tsv en-words.txt ja-words.txt ja-kana-native.txt ja-common-words.txt kanji.bin
  hanja.tsv ko-jamo-4gram.bin ja-shortcuts.tsv symbols.tsv emoji.tsv unicode-names.tsv key-models.bin)
list(TRANSFORM BADA_DATA_NAMES PREPEND "${BADA_ROOT}/engine/data/" OUTPUT_VARIABLE BADA_DATA)
file(GLOB BADA_DATA_LICENSES "${BADA_ROOT}/engine/data/LICENSE-*.txt")
# Installed once, for both frontends, which map the files: $BADA_DATA_DIR names another folder.
set(BADA_DATA_DIR "${CMAKE_INSTALL_FULL_DATADIR}/bada/data")
target_compile_definitions(bada_core PRIVATE BADA_DATA_DIR="${BADA_DATA_DIR}")
install(FILES ${BADA_DATA} ${BADA_DATA_LICENSES} DESTINATION ${CMAKE_INSTALL_DATADIR}/bada/data)

# The command line (cli/) and the settings page it hosts (settings/): `bada settings` serves the
# page and opens the browser. The page is found beside the binary, in ../share/bada/settings.
if(BADA_CLI)
  find_program(CARGO cargo REQUIRED)
  if(DEFINED ENV{CARGO_TARGET_DIR})
    set(BADA_CLI_BIN "$ENV{CARGO_TARGET_DIR}/release/bada")
  else()
    set(BADA_CLI_BIN "${BADA_ROOT}/target/release/bada")
  endif()
  add_custom_target(bada_cli ALL
    COMMAND ${CARGO} build --release -p bada-cli
    WORKING_DIRECTORY "${BADA_ROOT}"
    BYPRODUCTS "${BADA_CLI_BIN}"
    COMMENT "cargo build --release -p bada-cli"
    VERBATIM)
  # One cargo at a time in the workspace's target folder.
  if(TARGET bada_engine_build)
    add_dependencies(bada_cli bada_engine_build)
  endif()
  file(GLOB BADA_PAGE "${BADA_ROOT}/settings/src/*")
  install(PROGRAMS "${BADA_CLI_BIN}" DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${BADA_PAGE} DESTINATION ${CMAKE_INSTALL_DATADIR}/bada/settings)
  install(FILES src/com.teammirae.bada.settings.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

# What both frontends share beyond the core: the published state.
add_library(bada_common STATIC src/common/publish.cpp)
target_include_directories(bada_common PUBLIC src/common)
target_link_libraries(bada_common PUBLIC bada_core PkgConfig::DBUS)

if(BADA_FCITX5)
  find_package(Fcitx5Core 5.0 REQUIRED)
  add_library(bada-fcitx5 MODULE src/fcitx5/bada_fcitx5.cpp)
  target_link_libraries(bada-fcitx5 PRIVATE Fcitx5::Core bada_common)
  # The engine's symbols stay inside the addon.
  target_link_options(bada-fcitx5 PRIVATE "LINKER:--exclude-libs,ALL")
  set_target_properties(bada-fcitx5 PROPERTIES PREFIX "lib")
  set(BADA_FCITX5_ADDONDIR "${CMAKE_INSTALL_LIBDIR}/fcitx5" CACHE STRING "Where Fcitx5 loads addons from")
  configure_file(src/fcitx5/bada-addon.conf.in fcitx5/addon/bada.conf @ONLY)
  configure_file(src/fcitx5/bada-im.conf.in fcitx5/inputmethod/bada.conf @ONLY)
  install(TARGETS bada-fcitx5 DESTINATION ${BADA_FCITX5_ADDONDIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fcitx5/addon/bada.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fcitx5/inputmethod/bada.conf
          DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod)
endif()

if(BADA_IBUS)
  pkg_check_modules(IBUS REQUIRED IMPORTED_TARGET ibus-1.0)
  add_executable(ibus-engine-bada src/ibus/bada_ibus.cpp)
  target_link_libraries(ibus-engine-bada PRIVATE PkgConfig::IBUS bada_common)
  set(BADA_IBUS_EXEC "${CMAKE_INSTALL_FULL_LIBEXECDIR}/ibus-engine-bada")
  configure_file(src/ibus/com.teammirae.Bada.xml.in ibus/com.teammirae.Bada.xml @ONLY)
  install(TARGETS ibus-engine-bada DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ibus/com.teammirae.Bada.xml
          DESTINATION ${CMAKE_INSTALL_DATADIR}/ibus/component)
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

  # The core, typed into the linked engine: settings, the key flow, `types`, the state.
  add_executable(bada_core_tests tests/core_tests.cpp)
  target_link_libraries(bada_core_tests PRIVATE bada_common)
  target_compile_definitions(bada_core_tests PRIVATE BADA_ROOT="${BADA_ROOT}")
  add_test(NAME core COMMAND bada_core_tests)

  if(BADA_FCITX5)
    # The addon in a headless Fcitx5, driven through its test frontend.
    find_package(Fcitx5Module REQUIRED COMPONENTS TestFrontend)
    add_executable(bada_fcitx5_e2e tests/fcitx5_e2e.cpp)
    target_link_libraries(bada_fcitx5_e2e PRIVATE Fcitx5::Core Fcitx5::Module::TestFrontend bada_core)
    target_compile_definitions(bada_fcitx5_e2e PRIVATE
      BADA_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
      BADA_TEST_ADDON_DIR="${CMAKE_CURRENT_BINARY_DIR}"
      BADA_TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/fcitx5")
    add_dependencies(bada_fcitx5_e2e bada-fcitx5)
    add_test(NAME fcitx5_e2e COMMAND bada_fcitx5_e2e)
  endif()
endif()
