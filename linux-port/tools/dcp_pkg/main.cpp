// dcp_pkg: reads a PS4 fake package (fPKG) and unpacks it into a game folder, so a player does not extract it by hand.
// A separate program on purpose: the extractor (pkg_extract.*, crypto*) is GPL-2.0-or-later code from shadPS4, and this
// keeps it out of the other binaries of the repository (see README.md here). Run as a process, never linked.
//
//   dcp_pkg info    <game.pkg>                     title, ids, version and file list
//   dcp_pkg extract <game.pkg> <out folder>        writes the game folder; "progress <done> <total>" lines on stdout
//
// Exit status: 0 done, 1 not a usable package (the reason is on stderr), 2 usage, 3 cancelled / write failed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "pkg_extract.h"

namespace {

const char* const kReasons[] = {"ok", "not a PS4 package", "unsupported package (console-encrypted or unknown layout)",
                                "the package is incomplete", "could not read the package", "could not write the output",
                                "cancelled"};

int fail(const osl::PackageResult& r) {
    std::fprintf(stderr, "dcp_pkg: %s%s%s\n", kReasons[r.code], r.detail.empty() ? "" : ": ", r.detail.c_str());
    return r.code == osl::PackageResult::kWriteFailed || r.code == osl::PackageResult::kCancelled ? 3 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const bool info = argc == 3 && !std::strcmp(argv[1], "info");
    const bool extract = argc == 4 && !std::strcmp(argv[1], "extract");
    if (!info && !extract) {
        std::fprintf(stderr, "usage: dcp_pkg info <game.pkg> | dcp_pkg extract <game.pkg> <out folder>\n");
        return 2;
    }
    osl::Package pkg;
    if (auto r = pkg.Open(argv[2]); !r.ok()) return fail(r);
    if (auto r = pkg.LoadFiles(); !r.ok()) return fail(r);
    if (info) {
        std::printf("content id: %s\n%s", pkg.content_id().c_str(), pkg.sfo().c_str());
        std::printf("files: %zu, %llu bytes\n", pkg.file_count(), (unsigned long long)pkg.total_bytes());
        for (auto& f : pkg.Files()) std::printf("%12llu  %s\n", (unsigned long long)f.size, f.path.c_str());
        return 0;
    }
    uint64_t last = ~0ull;
    auto r = pkg.Extract(argv[3], [&](uint64_t done, uint64_t total) {
        const uint64_t pct = total ? done * 100 / total : 100;
        if (pct != last) std::printf("progress %llu %llu\n", (unsigned long long)done, (unsigned long long)total), std::fflush(stdout);
        last = pct;
        return true;
    });
    if (!r.ok()) return fail(r);
    return 0;
}
