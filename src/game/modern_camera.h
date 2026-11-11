// The quest camera's settings (CFG > Camera Settings; modern_camera.cpp): the Modern Camera, a free orbit camera on
// quests and in the field (off by default), and the camera's distance from the hunter (issue #17).
#pragma once

#include <cstdint>

namespace mh3u::game {

void register_modern_camera();
void set_modern_camera(bool on);         // any thread
void set_camera_distance(float factor);  // 1 = the game's own distance; any thread
// The host's direct mouse look (runtime/input: input::take_look / set_direct_look), set at boot.
struct ModernCameraMouse {
    void (*take)(float& x, float& y) = nullptr;  // degrees since the last call, right-stick directions (y up)
    void (*direct)(bool on) = nullptr;           // the mouse turns the camera itself, not the right stick
};
void set_modern_camera_mouse(ModernCameraMouse mouse);

// camera_speed.cpp's camera input hooks (they own those overrides) hand over to the mod through these.
// Around the game's camera up / down step (FUN_02286890; r4 = block):
struct ModernPitchSave {
    bool on = false;  // the mod is on: the step was marked
    uint8_t mark = 0, level = 0, reset = 0;
};
ModernPitchSave modern_before_pitch(uint8_t* base, uint32_t block);
void modern_after_pitch(uint8_t* base, uint32_t block, const ModernPitchSave& saved);
// This frame's camera is (or is about to be) the mod's: the game's own turn input and auto-turn stay out.
bool modern_camera_engaged();

}  // namespace mh3u::game
