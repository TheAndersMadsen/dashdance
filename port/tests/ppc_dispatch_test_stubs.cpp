// Link stubs for port_ppc_dispatch_test: the dispatch tests compile
// ppc_runtime.cpp for its interpreter/dispatch tables, which reference the
// guest RAM pointer the full host runtime defines. The dispatch tests never
// execute guest code that dereferences it.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"

namespace host {
uint8_t* ram = nullptr;
}
