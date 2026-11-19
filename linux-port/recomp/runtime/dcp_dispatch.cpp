#include "dcp_dispatch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dcp::rt {

u64 dcp_img = 0;

namespace {
struct Module {
    u64 base = 0, size = 0;
    const FnEntry* table = nullptr;
    size_t count = 0;
};
Module g_first;                // set_image / set_function_table
std::vector<Module> g_more;    // add_module; written before guest code runs, read-only afterwards

void default_foreign(Ctx*, u64 target) {
    std::fprintf(stderr, "dcp: call to 0x%llx, which is not a recompiled function\n", static_cast<unsigned long long>(target));
    std::abort();
}
ForeignHandler g_foreign = default_foreign;
std::unordered_map<u64, Fn> g_host;  // written before guest code runs, read-only afterwards

// A call and a jump dispatch the same way: the caller (or the code that pushed the return address) is already set up.
void dispatch(Ctx* c, u64 target) {
    if (Fn f = find_function(target)) return f(c);
    if (auto it = g_host.find(target); it != g_host.end()) return it->second(c);
    g_foreign(c, target);
}
}  // namespace

void set_image(u64 base, u64 size) { dcp_img = base, g_first.base = base, g_first.size = size; }
void set_function_table(const FnEntry* sorted, size_t count) { g_first.table = sorted, g_first.count = count; }
void add_module(u64 base, u64 size, const FnEntry* sorted, size_t count) { g_more.push_back({base, size, sorted, count}); }
void register_host_function(const void* host_fn, Fn thunk) { g_host[reinterpret_cast<u64>(host_fn)] = thunk; }
void set_foreign_handler(ForeignHandler handler) { g_foreign = handler ? handler : default_foreign; }

namespace {
std::map<std::pair<std::string, u64>, EntryHook*>& hook_slots() {
    static std::map<std::pair<std::string, u64>, EntryHook*> m;
    return m;
}
}  // namespace
void register_hook_slot(const char* module, u64 off, EntryHook* slot) { hook_slots()[{module, off}] = slot; }
bool set_entry_hook(const char* module, u64 off, EntryHook fn) {
    auto it = hook_slots().find({module, off});
    if (it == hook_slots().end()) return false;
    *it->second = fn;
    return true;
}

namespace {
std::map<std::pair<std::string, u64>, unsigned char*>& patch_slots() {
    static std::map<std::pair<std::string, u64>, unsigned char*> m;
    return m;
}
}  // namespace
void register_patch_slot(const char* module, u64 off, unsigned char* slot) { patch_slots()[{module, off}] = slot; }
bool set_patch(const char* module, u64 off, bool on) {
    auto it = patch_slots().find({module, off});
    if (it == patch_slots().end()) return false;
    *it->second = on ? 1 : 0;
    return true;
}

static Fn lookup(const Module& m, u64 target) {
    const u64 off = target - m.base;  // wraps for targets below the image, which the size check then rejects
    if (off >= m.size) return nullptr;
    const FnEntry* end = m.table + m.count;
    const FnEntry* it = std::lower_bound(m.table, end, off, [](const FnEntry& e, u64 v) { return e.off < v; });
    return it != end && it->off == off ? it->fn : nullptr;
}

Fn find_function(u64 target) {
    if (Fn f = lookup(g_first, target)) return f;
    for (const Module& m : g_more)
        if (Fn f = lookup(m, target)) return f;
    return nullptr;
}

void call_va(Ctx* c, u64 guest_va) { dispatch(c, dcp_img + guest_va); }
void call_ind(Ctx* c, u64 target) { dispatch(c, target); }
void jmp_ind(Ctx* c, u64 target) { dispatch(c, target); }

}  // namespace dcp::rt
