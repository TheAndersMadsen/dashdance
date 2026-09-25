// Slippi Jukebox: the netplay codes silence the game's own music and ask the EXI device to play
// the stage/menu HPS track instead (offset + size on the disc). This decodes the HPS (DSP-ADPCM,
// stereo, looping) and hands samples to the host audio mixer.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace slippi::jukebox {
// Lifecycle and command methods run on the EXI/host owner, never on the decoder.
void init();
void shutdown();                                       // cancel pending work and join the decoder
void start_song(uint32_t disc_offset, uint32_t size);   // CMD_PLAY_MUSIC
void stop();                                            // CMD_STOP_MUSIC
void set_melee_volume(uint8_t volume);                  // CMD_CHANGE_MUSIC_VOLUME (0..254)
void set_user_volume(int percent);                      // PC settings "Music" (0..100)
int user_volume();
// Mixes `frames` stereo 32 kHz samples into `out` (adds to what is there). Audio-thread safe.
void mix(int16_t* out, size_t frames, int master_volume);
}  // namespace slippi::jukebox
