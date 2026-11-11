# Hand-written game code replacing recompiled functions (config/overrides.txt lists them; CLAUDE.md conventions).
add_library(mh3u_game_src STATIC game/boot_prompts.cpp game/boot_prompts.h game/filters.cpp game/filters.h
  game/frame_limit.cpp game/frame_limit.h game/monster_hp.cpp game/monster_hp.h game/text_trace.cpp game/text_trace.h game/item_sets.cpp game/item_sets.h
  game/carving.cpp game/carving.h game/menu_mouse.cpp game/menu_mouse.h
  game/camera_speed.cpp game/camera_speed.h game/play_log.cpp game/play_log.h game/windpress.cpp game/windpress.h game/grab_escape.cpp game/grab_escape.h
  game/movie_skip.cpp game/movie_skip.h game/net_probe.cpp game/net_probe.h game/net_backend.cpp game/net_backend.h
  game/charm_probe.cpp game/charm_probe.h)
target_include_directories(mh3u_game_src PUBLIC game)
target_link_libraries(mh3u_game_src PUBLIC mh3u_recomp_game mh3u_core)
if(WIN32)
  target_link_libraries(mh3u_game_src PRIVATE ws2_32)  # net_backend.cpp
endif()
