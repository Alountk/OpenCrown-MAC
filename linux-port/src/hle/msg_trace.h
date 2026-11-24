// Debug aid: DCP_TRACE_MSG=1 logs each (message id, caller) the game asks the message lookup for, once, so a menu's texts can be
// matched to the code that builds it (the texts are in the msgsheet/*.fms files of the user's own dump). Recompiled build only.
#pragma once
#include <cstdint>

namespace dcp::msg_trace {

void install(uint8_t* eboot_base);  // no-op unless DCP_TRACE_MSG is set; it takes over the lookup site, so the no-skip warning text is off while it runs

}  // namespace dcp::msg_trace
