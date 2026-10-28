# Static recompiler: PPC (Espresso) -> C++. Phase 2, see docs/ROADMAP.md.
add_library(mh3u_recomp_core STATIC
  src/espresso.cpp
  src/espresso.h
  src/elf.cpp
  src/elf.h
  src/inflate.cpp
  src/inflate.h
  src/loader.cpp
  src/loader.h
  src/emitter.cpp
  src/emitter.h
)
target_include_directories(mh3u_recomp_core PUBLIC src include)

# Guest context / memory model used by generated code.
add_library(mh3u_ppc_context INTERFACE)
target_include_directories(mh3u_ppc_context INTERFACE include)

add_executable(espresso_test tests/espresso_test.cpp)
target_link_libraries(espresso_test PRIVATE mh3u_recomp_core)
add_test(NAME espresso_decode COMMAND espresso_test)

# Decodes every function body of the real executable; skipped (77) without the user's dump.
add_executable(decoder_coverage tests/decoder_coverage.cpp)
target_link_libraries(decoder_coverage PRIVATE mh3u_recomp_core)
add_test(NAME espresso_coverage COMMAND decoder_coverage "${MH3U_REPO_ROOT}")
set_tests_properties(espresso_coverage PROPERTIES SKIP_RETURN_CODE 77)

# Applies the RPL relocations to the real executable; skipped (77) without the user's dump.
add_executable(loader_check tests/loader_check.cpp)
target_link_libraries(loader_check PRIVATE mh3u_recomp_core)
add_test(NAME recomp_loader COMMAND loader_check "${MH3U_REPO_ROOT}")
set_tests_properties(recomp_loader PROPERTIES SKIP_RETURN_CODE 77)

# Emitter self-test: hand-encoded functions -> C++ at build time -> compiled and executed.
add_executable(emit_selftest_gen tests/emit_selftest_gen.cpp)
target_link_libraries(emit_selftest_gen PRIVATE mh3u_recomp_core)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/selftest_funcs.cpp
  COMMAND emit_selftest_gen ${CMAKE_CURRENT_BINARY_DIR}/selftest_funcs.cpp
  DEPENDS emit_selftest_gen tests/selftest_cases.h
  COMMENT "Emitting self-test functions")
add_executable(emit_selftest tests/emit_selftest.cpp ${CMAKE_CURRENT_BINARY_DIR}/selftest_funcs.cpp)
target_include_directories(emit_selftest PRIVATE tests)
target_link_libraries(emit_selftest PRIVATE mh3u_ppc_context mh3u_recomp_support)
add_test(NAME emit_selftest COMMAND emit_selftest)

# Real game code: recomp_emit writes recomp/out (emitted functions, stubs for the rest, import stubs, function
# table); everything is compiled into mh3u_recomp_game and linked by link_check. Empty without the user's dump.
add_library(mh3u_recomp_support STATIC support/unimplemented.cpp support/indirect.cpp support/imports.cpp)
target_link_libraries(mh3u_recomp_support PUBLIC mh3u_ppc_context)

# where the generated C++ goes (a Linux build in WSL keeps its own copy on the Linux disk)
set(MH3U_RECOMP_OUT ${CMAKE_SOURCE_DIR}/recomp/out CACHE PATH "Generated game code")
set(MH3U_RECOMP_FILES 64)
set(game_sources ${MH3U_RECOMP_OUT}/stubs.cpp ${MH3U_RECOMP_OUT}/imports.cpp ${MH3U_RECOMP_OUT}/function_table.cpp)
math(EXPR last "${MH3U_RECOMP_FILES} - 1")
foreach(i RANGE ${last})
  string(LENGTH "${i}" len)
  if(len EQUAL 1)
    set(i "0${i}")
  endif()
  list(APPEND game_sources ${MH3U_RECOMP_OUT}/recomp_${i}.cpp)
endforeach()
add_executable(recomp_emit tools/recomp_emit.cpp)
target_link_libraries(recomp_emit PRIVATE mh3u_recomp_core)
add_custom_command(
  OUTPUT ${game_sources} ${MH3U_RECOMP_OUT}/functions.h
  COMMAND recomp_emit ${MH3U_REPO_ROOT} ${MH3U_RECOMP_OUT} ${MH3U_RECOMP_FILES}
  DEPENDS recomp_emit ${MH3U_REPO_ROOT}/config/functions.csv ${MH3U_REPO_ROOT}/config/overrides.txt
  COMMENT "Recompiling the game into recomp/out")
add_library(mh3u_recomp_game STATIC ${game_sources})
target_include_directories(mh3u_recomp_game PUBLIC ${MH3U_RECOMP_OUT})
target_link_libraries(mh3u_recomp_game PUBLIC mh3u_ppc_context mh3u_recomp_support)
if(MSVC)
  target_compile_options(mh3u_recomp_game PRIVATE /bigobj /MP)
endif()

add_executable(link_check tests/link_check.cpp)
target_link_libraries(link_check PRIVATE mh3u_recomp_game)
add_test(NAME recomp_link COMMAND link_check)
set_tests_properties(recomp_link PROPERTIES SKIP_RETURN_CODE 77)

# Disassembler for investigating the binary (names imports and symbols).
add_executable(recomp_disasm tools/recomp_disasm.cpp)
target_link_libraries(recomp_disasm PRIVATE mh3u_recomp_core)
