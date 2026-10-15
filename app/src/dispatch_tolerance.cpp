// nfscarbon-nx: unregistered indirect guest calls are logged and no-op'd
// instead of a fatal error.
//
// From nfs-carbon-recomp/src/dispatch_tolerance.cpp. Each new target is
// appended to unregistered_targets.txt next to the executable (on the Switch,
// sdmc:/switch/nfscarbon-nx/) and to the log; add them to
// nfscarbon_manifest.toml as function hints and run codegen again.
//
// The SDK's own ResolveIndirectFunction is weak in this tree
// (sdk/src/system/function_dispatcher.cpp) so this one replaces it, also on the
// Switch, where the runtime is linked with --whole-archive.

#include <cstdio>
#include <mutex>
#include <set>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(nfscarbon_tolerate_unregistered_calls, true, "Carbon",
                    "Log and skip calls to guest functions the recompiler did not register "
                    "instead of stopping the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

void NoopTrap(PPCContext& ctx, uint8_t* /*base*/) {
  static std::mutex mutex;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (!REXCVAR_GET(nfscarbon_tolerate_unregistered_calls)) {
    REX_FATAL("Call to invalid or unregistered function at guest address 0x{:08X}",
              ctx.last_indirect_target);
  }
  if (seen.insert(ctx.last_indirect_target).second) {
    REXLOG_WARN("[dispatch] unregistered indirect call to 0x{:08X} skipped (lr 0x{:08X})",
                ctx.last_indirect_target, uint32_t(ctx.lr));
    const auto path = rex::filesystem::GetExecutableFolder() / "unregistered_targets.txt";
    if (std::FILE* f = std::fopen(path.string().c_str(), "a")) {
      std::fprintf(f, "0x%08X\n", (unsigned)ctx.last_indirect_target);
      std::fclose(f);
    }
  }
  ctx.r3.u64 = 0;
}

}  // namespace

namespace rex::runtime {

::PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  if (Runtime* rt = Runtime::instance()) {
    if (FunctionDispatcher* d = rt->function_dispatcher()) {
      if (::PPCFunc* f = d->GetFunction(guest_address)) {
        return f;
      }
    }
  }
  return &NoopTrap;
}

}  // namespace rex::runtime
