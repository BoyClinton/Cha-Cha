// Skippable cutscenes. MH3U's story and monster cutscenes are pre-rendered movies (nativeCafe/movie/NN.mp4) played
// by sMH3GMovie (singleton *(0x10314A70)): +0x34 is the playing uMovie, FUN_021571D0 asks "still playing?" (a movie is
// set and its player state uMovie+0x32C has not reached 6, the end), FUN_02156C28 stops it (player stop, unit
// released, +0x34 cleared; returns at once when no movie is set). The scenes poll "still playing?" each frame and, on
// false, go on as after the movie's end (FUN_02026020 then stops it, FUN_02115D5C clears its flag).
//
// Replaces (status: wip - read in the recompiled code 2026-11-04; checked headless that the game boots, the skip
// itself to be seen in a window): FUN_021571D0 sMH3GMovie::isPlaying? - the original runs; while a movie plays, a
// new press of + (the skip button) stops it with FUN_02156C28 and answers "not playing", so the scene ends the way
// it does after the last frame. The game's other callers of the stop function run as before (stopping twice is
// harmless).
#include "movie_skip.h"

#include <cstdio>

#include "functions.h"

namespace mh3u::game {
namespace {

bool (*g_take_skip)() = nullptr;

PPC_FUNC(movie_playing) {
    const uint32_t movies = static_cast<uint32_t>(ctx.r[3]);
    sub_021571D0__orig(ctx, base);
    if (ctx.r[3] == 0 || !g_take_skip || !g_take_skip()) return;
    std::printf("movie: skipped by the player\n");
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    ctx.r[3] = movies;
    sub_02156C28(ctx, base);  // stop: the movie, its unit, sMH3GMovie+0x34
    ctx.lr = lr;
    ctx.r[3] = 0;  // not playing: the scene goes on as after the movie
}

}  // namespace

void register_movie_skip(bool (*take_skip)()) {
    g_take_skip = take_skip;
    ppc::set_override(0x021571D0, &movie_playing);
}

}  // namespace mh3u::game
