// Slippi code tables embedded by the recompiler (port/generated/gecko_data.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace gecko {
extern const uint8_t codehandler_bin[]; extern const size_t codehandler_bin_size;
extern const uint8_t bootloader_gct[];  extern const size_t bootloader_gct_size;
extern const uint8_t slippi_gct[];      extern const size_t slippi_gct_size;
struct CodeTable { const uint8_t* data; size_t size; };
extern const CodeTable slippi_gct_options[4];
struct Write { uint32_t addr; uint32_t size; const uint8_t* data; };
extern const Write boot_writes[];       extern const size_t boot_writes_count;
struct HookInstall { uint32_t hook; uint32_t cave_addr; uint32_t words; };
extern const HookInstall boot_hooks[];  extern const size_t boot_hooks_count;
extern const uint32_t gct_base_used;
// Run-time optional codes (compiled both ways by the recompiler; see gecko.py RUNTIME_OPTIONAL).
extern bool option_widescreen;                 // read by translated code at the patched instructions
extern bool option_flash_failed_lcancel;
extern const uint32_t optional_gct_offset;     // where the optional codes start inside slippi_gct
struct OptionalWrite { uint32_t addr; uint32_t size; const uint8_t* patched; const uint8_t* original; const bool* enabled; };
extern const OptionalWrite optional_writes[]; extern const size_t optional_writes_count;
}  // namespace gecko
