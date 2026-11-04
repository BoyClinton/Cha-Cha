// The game camera's turn speed and easing made independent of the frame rate (camera_speed.cpp).
#pragma once

namespace mh3u::game {

void register_camera_speed();
// the Camera speed setting (0.3 .. 1.0, 1 = the game's): scales the turn / tilt steps; callable from any thread
void set_camera_speed(float speed);

}  // namespace mh3u::game
