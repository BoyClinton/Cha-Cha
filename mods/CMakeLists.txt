# Mods made with Cha-Cha's plugin API (include/chacha_plugin.h, docs/modding.md). Each builds into
# <build>/mods/<mod>/ as a ready mod folder: copy that folder into <data folder>/mods.
function(chacha_mod_plugin mod target)
  add_library(${target} MODULE ${ARGN})
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods/${mod}/plugins
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/mods/${mod}/plugins
    LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/mods/${mod}/plugins)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${mod}/mod.ini ${CMAKE_BINARY_DIR}/mods/${mod}/mod.ini COPYONLY)
endfunction()

chacha_mod_plugin(damage-numbers damage_numbers damage-numbers/src/damage_numbers.cpp)
chacha_mod_plugin(true-attack true_attack true-attack/src/true_attack.cpp)
chacha_mod_plugin(key-quests key_quests key-quests/src/key_quests.cpp)
