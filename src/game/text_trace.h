// Game text lookups (text_trace.cpp): MH3U_TRACE_TEXT logs which code asks for which text; set_text replaces entries.
#pragma once

#include <cstdint>
#include <string>

namespace mh3u::game {

void register_text_trace();
// The game gets this text for (group, index) from now on - asked from `caller` only (the return address of the lookup),
// or from anywhere with caller 0; any thread, may be changed again. Groups: 0x04 item names (index = item id),
// 0x2E GUI\font\lobby\Shop, 0x2F lobby\Help, 0x35 lobby\NpcName (docs/research-log.md, "Game text lookups").
void set_text(uint32_t group, uint32_t index, const std::string& utf8, uint32_t caller = 0);

}  // namespace mh3u::game
