// macOS (x86_64, run natively on Intel or under Rosetta 2) shims for the Linux-only calls this tree uses.
// On Linux this header is empty, so including it is always safe.
#pragma once

#ifdef __APPLE__
// Two ways to run on a Mac: the x86-64 build runs the PS4's code directly (Rosetta 2 on Apple Silicon); the arm64 build runs
// code recompiled from it and must say so with DCP_RECOMPILED (see odd/tasks/arm64-static-recomp.md).
#if !defined(__x86_64__) && !defined(DCP_RECOMPILED)
#error "Build with -DCMAKE_OSX_ARCHITECTURES=x86_64 (Rosetta 2), or for arm64 with -DDCP_RECOMPILED=ON (recompiled game code)"
#endif

#include <cstddef>
#include <cstdint>

#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

// macOS has no MAP_FIXED_NOREPLACE: without MAP_FIXED the address is only a hint, which is the pre-4.17 Linux behaviour
// every caller already handles (they compare the returned address and unmap on a mismatch).
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

// No SOCK_CLOEXEC / accept4 either. The flag is dropped; this process does not exec children that could inherit sockets.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
inline int accept4(int fd, sockaddr* a, socklen_t* len, int) { return accept(fd, a, len); }

// getentropy() is capped at 256 bytes per call.
inline ssize_t getrandom(void* p, size_t n, unsigned) {
    if (n > 256) n = 256;
    return getentropy(p, n) == 0 ? ssize_t(n) : -1;
}

inline long gettid() {
    uint64_t t = 0;
    pthread_threadid_np(nullptr, &t);
    return long(t);
}

// mincore() takes char* on macOS and unsigned char* on Linux.
using dcp_mincore_t = char;

namespace dcp::platform {

// Copy to or from host memory without faulting (Linux uses process_vm_readv / writev on ourselves).
inline bool safe_read(uint64_t addr, void* out, size_t n) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), addr, n, reinterpret_cast<mach_vm_address_t>(out), &got) ==
               KERN_SUCCESS &&
           got == n;
}
inline bool safe_write(uint64_t addr, const void* in, size_t n) {
    return mach_vm_write(mach_task_self(), addr, reinterpret_cast<vm_offset_t>(in), mach_msg_type_number_t(n)) ==
           KERN_SUCCESS;
}

#if defined(__x86_64__)
// Signal-handler register access with the Linux REG_* names, so fault handlers read the same on both systems.
enum Reg { REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RDI, REG_RSI, REG_RBP, REG_RSP, REG_R8, REG_R9, REG_R10, REG_R11,
           REG_R12, REG_R13, REG_R14, REG_R15, REG_RIP, REG_EFL, REG_ERR };

class GRegs {
public:
    explicit GRegs(void* ucv) : mc_(static_cast<ucontext_t*>(ucv)->uc_mcontext) {}
    uint64_t& operator[](Reg r) const {
        auto& s = mc_->__ss;
        switch (r) {
        case REG_RAX: return s.__rax;
        case REG_RBX: return s.__rbx;
        case REG_RCX: return s.__rcx;
        case REG_RDX: return s.__rdx;
        case REG_RDI: return s.__rdi;
        case REG_RSI: return s.__rsi;
        case REG_RBP: return s.__rbp;
        case REG_RSP: return s.__rsp;
        case REG_R8: return s.__r8;
        case REG_R9: return s.__r9;
        case REG_R10: return s.__r10;
        case REG_R11: return s.__r11;
        case REG_R12: return s.__r12;
        case REG_R13: return s.__r13;
        case REG_R14: return s.__r14;
        case REG_R15: return s.__r15;
        case REG_RIP: return s.__rip;
        case REG_EFL: return s.__rflags;
        case REG_ERR: break;
        }
        static thread_local uint64_t err;  // __es.__err is 32-bit; copy it so callers can read it as 64-bit
        err = mc_->__es.__err;
        return err;
    }

private:
    mcontext_t mc_;
};
}  // namespace dcp::platform

using dcp::platform::GRegs;
using enum dcp::platform::Reg;
using dcp::platform::Reg;
using greg_t = uint64_t;
#else
}  // namespace dcp::platform
#endif  // x86_64: the arm64 build has no x86 register state to show; code that reads it is compiled out

#else  // Linux

using dcp_mincore_t = unsigned char;

#endif
