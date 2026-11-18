#include "dcp_dispatch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dcp::rt {

u64 dcp_img = 0;

namespace {
u64 g_size = 0;
const FnEntry* g_table = nullptr;
size_t g_count = 0;

void default_foreign(Ctx*, u64 target) {
    std::fprintf(stderr, "dcp: call to 0x%llx, which is not a recompiled function\n", static_cast<unsigned long long>(target));
    std::abort();
}
ForeignHandler g_foreign = default_foreign;

// A call and a jump dispatch the same way: the caller (or the code that pushed the return address) is already set up.
void dispatch(Ctx* c, u64 target) {
    if (Fn f = find_function(target)) return f(c);
    g_foreign(c, target);
}
}  // namespace

void set_image(u64 base, u64 size) { dcp_img = base, g_size = size; }
void set_function_table(const FnEntry* sorted, size_t count) { g_table = sorted, g_count = count; }
void set_foreign_handler(ForeignHandler handler) { g_foreign = handler ? handler : default_foreign; }

Fn find_function(u64 target) {
    const u64 off = target - dcp_img;  // wraps for targets below the image, which the size check then rejects
    if (off >= g_size) return nullptr;
    const FnEntry* end = g_table + g_count;
    const FnEntry* it = std::lower_bound(g_table, end, off, [](const FnEntry& e, u64 v) { return e.off < v; });
    return it != end && it->off == off ? it->fn : nullptr;
}

void call_va(Ctx* c, u64 guest_va) { dispatch(c, dcp_img + guest_va); }
void call_ind(Ctx* c, u64 target) { dispatch(c, target); }
void jmp_ind(Ctx* c, u64 target) { dispatch(c, target); }

}  // namespace dcp::rt
