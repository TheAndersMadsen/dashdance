// Slippi replay playback: the EXI side of Slippi's Playback codes (port of CEXISlippi's
// prepareGameInfo / prepareFrameData / prepareIsStockSteal / prepareIsFileReady / gecko list).
// The playback build of the port (recompiled with the Slippi Playback code set) plays one .slp
// given with --replay, records it again through the normal recording path, and exits when done.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::playback {
void set_replay(const std::string& path);   // --replay <file.slp>
bool enabled();
void prepare_game_info(const uint8_t* payload, std::vector<uint8_t>& q);     // CMD_PREPARE_REPLAY
void prepare_frame_data(const uint8_t* payload, std::vector<uint8_t>& q);    // CMD_READ_FRAME
void prepare_is_stock_steal(const uint8_t* payload, std::vector<uint8_t>& q);// CMD_IS_STOCK_STEAL
void prepare_is_file_ready(std::vector<uint8_t>& q);                         // CMD_IS_FILE_READY
void prepare_gecko_codes(std::vector<uint8_t>& q);                           // CMD_GET_GECKO_CODES
void note_gecko_list_dma(uint32_t addr, uint32_t size);                      // where the game put the list
void note_game_end();                                                        // the match ended: close the player
}  // namespace slippi::playback
