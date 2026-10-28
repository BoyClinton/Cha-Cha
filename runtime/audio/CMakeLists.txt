# Audio: AX (snd_core) / AXFX (snd_user) HLE; software mixer -> SDL3 audio stream (docs/runtime.md § Audio).

add_library(mh3u_audio STATIC ax.cpp ax.h)
target_include_directories(mh3u_audio PUBLIC .)
target_link_libraries(mh3u_audio PUBLIC mh3u_os PRIVATE SDL3::SDL3)
