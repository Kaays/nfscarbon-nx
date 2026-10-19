// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: profile dump of the instrumented build for PGO (port of nfsmw-nx's nfsmw_pgo.cpp).
//
// Only compiled with NFSCARBON_PGO=generate (app/CMakeLists.txt). GCC embeds in the executable, for each
// object, the path NFSCARBON_PGO_DIR/CMakeFiles#...#file.gcda (the pgo/ folder of the repository on the
// PC that built it). That cannot be opened on the Switch, so a --wrap of fopen sends it to
// sdmc:/switch/nfscarbon-nx/pgo/CMakeFiles#...#file.gcda.
//
// The Switch does not always close the program cleanly (the HOME menu kills it), so a thread dumps the
// profile every 3 minutes and resets the counters. libgcov adds to what each .gcda already holds, so
// several dumps (and several sessions) add up.
#if defined(NFSCARBON_PGO_GENERATE) && defined(__SWITCH__)

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace {
constexpr char kSdDir[] = "sdmc:/switch/nfscarbon-nx/pgo";
}

extern "C" {
FILE* __real_fopen(const char* path, const char* mode);
void __gcov_dump(void);
void __gcov_reset(void);

FILE* __wrap_fopen(const char* path, const char* mode) {
  static constexpr char kPrefix[] = NFSCARBON_PGO_DIR;
  if (path && std::strncmp(path, kPrefix, sizeof(kPrefix) - 1) == 0) {
    const char* rest = path + sizeof(kPrefix) - 1;
    while (*rest == '\\' || *rest == '/') {
      ++rest;
    }
    std::string moved = std::string(kSdDir) + "/";
    for (; *rest; ++rest) {
      moved += (*rest == '\\') ? '/' : *rest;
    }
    return __real_fopen(moved.c_str(), mode);
  }
  return __real_fopen(path, mode);
}
}

namespace {
Thread g_pgo_thread;

void PgoThread(void*) {
  for (int dump = 1;; ++dump) {
    svcSleepThread(180LL * 1000 * 1000 * 1000);
    mkdir(kSdDir, 0777);
    const u64 before = armGetSystemTick();
    __gcov_dump();
    __gcov_reset();
    const double ms = double(armTicksToNs(armGetSystemTick() - before)) / 1e6;
    if (FILE* f = __real_fopen("sdmc:/switch/nfscarbon-nx/pgo/dumps.txt", "a")) {
      std::fprintf(f, "dump %d: %.0f ms\n", dump, ms);
      std::fclose(f);
    }
  }
}

__attribute__((constructor)) void StartPgo() {
  // The game's normal priority (0x3B), any core: it only works once every 3 minutes.
  if (R_SUCCEEDED(threadCreate(&g_pgo_thread, PgoThread, nullptr, nullptr, 64 * 1024, 0x3B, -2))) {
    threadStart(&g_pgo_thread);
  }
}
}  // namespace

#endif
