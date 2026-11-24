// See msg_trace.h. The message lookup (eboot 0x591450, esi = id) is already a site of hooks/eboot_patches.txt; the hook runs at
// its entry, so [rsp] is the caller's return address.
#include "msg_trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

#ifndef __x86_64__
#include "dcp_dispatch.h"
#endif

namespace dcp::msg_trace {
namespace {
#ifndef __x86_64__
constexpr uint32_t kLookupSite = 0x591450, kSetTextSite = 0x5915d0, kSetTextSlotSite = 0x591730;
uint64_t g_base = 0;

rt::u64 log_site(rt::Ctx* c, const char* what, uint32_t id) {
    static std::mutex mu;
    static std::set<std::pair<uint32_t, uint64_t>> seen;
    uint64_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(c->r[rt::RSP]), sizeof ret);
    std::lock_guard<std::mutex> lk(mu);
    if (seen.insert({id, ret}).second)
        std::fprintf(stderr, "msg: %s id %d (0x%x) from eboot+0x%llx\n", what, int32_t(id), id, (unsigned long long)(ret - g_base));
    return 0;  // the function runs as usual
}
rt::u64 site_lookup(rt::Ctx* c) { return log_site(c, "lookup", uint32_t(c->r[rt::RSI])); }
rt::u64 site_set_text(rt::Ctx* c) { return log_site(c, "SetText", uint32_t(c->r[rt::RSI])); }
rt::u64 site_set_text_slot(rt::Ctx* c) { return log_site(c, "SetText(slot)", uint32_t(c->r[rt::RDX])); }
#endif
}  // namespace

void install(uint8_t* base) {
    if (!std::getenv("DCP_TRACE_MSG")) return;
#ifndef __x86_64__
    g_base = reinterpret_cast<uint64_t>(base);
    const bool ok = rt::set_site_hook("eboot", kLookupSite, site_lookup) && rt::set_site_hook("eboot", kSetTextSite, site_set_text) &&
                    rt::set_site_hook("eboot", kSetTextSlotSite, site_set_text_slot);
    if (!ok) {
        std::fprintf(stderr, "msg_trace: the recompiled eboot lacks these site hooks (recompile with hooks/eboot_patches.txt)\n");
        return;
    }
    std::fprintf(stderr, "msg_trace: logging message ids and their callers\n");
#else
    (void)base;
    std::fprintf(stderr, "msg_trace: only in the recompiled build\n");
#endif
}

}  // namespace dcp::msg_trace
