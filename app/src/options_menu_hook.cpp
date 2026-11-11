// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: port settings as rows of the game's own Options screen (USA default.xex).
//
// The options screen is filled row by row by sub_82347400 (screen, id): it names the row
// "OPTION_<n>" from a counter at screen+0x160 (the name is hashed: h = h * 33 + c from -1),
// builds a widget for the id and hands it to sub_82313730 (screen, name hash, widget). The
// on/off widget is 0x80 bytes, built by sub_82345AE0 (this, &label hash, id, on text hash,
// off text hash, enabled): +0xC label hash, +0x1C value text object, +0x64 enabled, +0x74 id,
// +0x78 / +0x7C the two value texts. Its Draw (vtable +4, sub_82345C90), Act (+0x10,
// sub_82345B68: pad left/right flips the value) and Reset (+0x20, sub_82345D38) read and
// write the game's settings by id, in a table indexed by id / 1000. Our rows use ids the game
// does not have (kIdBase..), so those three are wrapped: for our ids they step our own value.
//
// Labels and values are text hashes of our own. sub_8231F8F8 turns a text hash into the string
// of the current language (it searches the three loaded string tables); for our hashes it
// answers with our strings, kept in guest memory taken from the game's allocator.
//
// The rows are appended after the last row of the Video screen (kAfterId; its only row is the
// brightness, 0x138D. Gameplay ends with 0x13B0). Values are kept in
// nfscarbon_options.txt next to the NRO, loaded at the first text lookup of the session, and
// applied by setting the matching cvars.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "../generated/default/nfscarbon_pch.h"

REXCVAR_DEFINE_BOOL(nfscarbon_options_menu, true, "Carbon",
                    "Port settings as rows of the game's Options screen");
REXCVAR_DEFINE_BOOL(nfscarbon_options_menu_log, true, "Carbon",
                    "Log the ids of the Options rows the game adds (to find the screens)");

extern "C" void __imp__sub_82347400(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82345C90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82345B68(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82345D38(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8231F8F8(PPCContext& __restrict ctx, uint8_t* base);
REX_EXTERN(sub_821A3320);  // allocator (size, 0, 0, 0)
REX_EXTERN(sub_82345AE0);  // on/off widget constructor
REX_EXTERN(sub_82313730);  // add a widget to the screen
REX_EXTERN(sub_822FA300);  // set a text object's string by hash

namespace {

constexpr uint32_t kAfterId = 0x138D;    // Video screen: its last (only) row
constexpr uint32_t kIdBase = 18900;      // our ids (id / 1000 = 18: the table's last slot)
constexpr uint32_t kPadLeft = 0x9120409Eu;
constexpr uint32_t kPadRight = 0xB5971BF1u;

struct Row {
  const char* key;    // in nfscarbon_options.txt
  const char* label;  // shown in the menu
  const char* on;     // value texts
  const char* off;
  const char* cvar;   // applied with these values
  const char* cvar_on;
  const char* cvar_off;
  bool dflt;
  bool value;
};
Row g_rows[] = {
    {"motion_blur", "Motion Blur", "On", "Off", "carbon_gpu_motion_blur", "true", "false", true, true},
    // nfscarbon_lod_bias: the game's own detail, or small far objects dropping out sooner (fewer
    // draws in crowded races; the shipped toml has 0.6).
    {"scene_detail", "Scene Detail", "Full", "Reduced", "nfscarbon_lod_bias", "1.0", "0.6", false, false},
    // carbon_gpu_reflection_faces: the six faces of the car reflection cube every frame, or one
    // face a frame (each face fresh every 6 frames; fewer draws).
    {"car_reflections", "Car Reflections", "Full", "Reduced", "carbon_gpu_reflection_faces", "6", "1", true, true},
    // carbon_gpu_mirror_half_rate: the rear-view mirror (and the boss picture-in-picture box)
    // every frame, or every other frame.
    {"mirror", "Rear-View Mirror", "Full Rate", "Half Rate", "carbon_gpu_mirror_half_rate", "false", "true", true, true},
};
constexpr uint32_t kRowCount = sizeof(g_rows) / sizeof(g_rows[0]);

// Text hashes of our strings: per row its label, on text and off text.
constexpr uint32_t kTextBase = 0x7A5E0100u;
uint32_t LabelHash(uint32_t row) { return kTextBase + row * 3; }
uint32_t OnHash(uint32_t row) { return kTextBase + row * 3 + 1; }
uint32_t OffHash(uint32_t row) { return kTextBase + row * 3 + 2; }

std::mutex g_mutex;
uint32_t g_strings = 0;  // guest block: 3 strings of kStringBytes per row
constexpr uint32_t kStringBytes = 48;
uint32_t g_scratch = 0;  // guest word for the constructor's label pointer
std::atomic<bool> g_loaded{false};

std::filesystem::path OptionsPath() {
  return rex::filesystem::GetExecutableFolder() / "nfscarbon_options.txt";
}

void Apply(const Row& r) { rex::cvar::SetFlagByName(r.cvar, r.value ? r.cvar_on : r.cvar_off); }

void Save() {
  std::ofstream f(OptionsPath(), std::ios::trunc);
  if (!f) {
    REXLOG_WARN("[menu] cannot write {}", OptionsPath().string());
    return;
  }
  f << "# nfscarbon-nx: settings changed in the game's Options screen (they win over nfscarbon.toml)\n";
  for (const Row& r : g_rows) f << r.key << "=" << (r.value ? 1 : 0) << "\n";
}

// Values from the file; rows not in it keep the toml's value (read back from the cvar).
void LoadOnce() {
  if (g_loaded.exchange(true)) return;
  for (Row& r : g_rows) {
    std::string v = rex::cvar::GetFlagByName(r.cvar);
    if (v.empty()) {
      r.value = r.dflt;
    } else if (v == r.cvar_on || v == r.cvar_off) {
      r.value = v == r.cvar_on;
    } else if (v == "true" || v == "false") {
      r.value = r.dflt;
    } else {
      r.value = std::strtod(v.c_str(), nullptr) >= std::strtod(r.cvar_on, nullptr) - 1e-6;
    }
  }
  std::ifstream f(OptionsPath());
  std::string line;
  uint32_t n = 0;
  while (f && std::getline(f, line)) {
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    for (Row& r : g_rows) {
      if (k == r.key) {
        r.value = v == "1" || v == "true";
        Apply(r);
        ++n;
      }
    }
  }
  REXLOG_INFO("[menu] {} setting(s) from {}", n, OptionsPath().string());
}

int RowOf(uint32_t id) {
  return id >= kIdBase && id < kIdBase + kRowCount ? int(id - kIdBase) : -1;
}

void PutString(uint8_t* base, uint32_t at, const char* s) {
  uint32_t i = 0;
  for (; s[i] && i + 1 < kStringBytes; ++i) REX_STORE_U8(at + i, uint8_t(s[i]));
  REX_STORE_U8(at + i, 0);
}

// Guest memory for our strings and the scratch word, taken once from the game's allocator.
bool EnsureGuestMemory(PPCContext& ctx, uint8_t* base) {
  if (g_strings) return true;
  ctx.r3.u64 = kRowCount * 3 * kStringBytes + 16;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  sub_821A3320(ctx, base);
  uint32_t block = ctx.r3.u32;
  if (!block) return false;
  for (uint32_t i = 0; i < kRowCount; ++i) {
    PutString(base, block + 16 + (i * 3 + 0) * kStringBytes, g_rows[i].label);
    PutString(base, block + 16 + (i * 3 + 1) * kStringBytes, g_rows[i].on);
    PutString(base, block + 16 + (i * 3 + 2) * kStringBytes, g_rows[i].off);
  }
  g_scratch = block;
  g_strings = block + 16;
  return true;
}

uint32_t StringFor(uint32_t hash) {
  if (!g_strings || hash < kTextBase || hash >= kTextBase + kRowCount * 3) return 0;
  return g_strings + (hash - kTextBase) * kStringBytes;
}

uint32_t NameHash(uint32_t n) {
  char name[32];
  std::snprintf(name, sizeof(name), "OPTION_%u", n);
  uint32_t h = 0xFFFFFFFFu;
  for (const char* p = name; *p; ++p) h = h * 33 + uint8_t(*p);
  return h;
}

void AddRow(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t row) {
  uint32_t counter = REX_LOAD_U32(screen + 0x160) + 1;
  REX_STORE_U32(screen + 0x160, counter);
  uint32_t name = NameHash(counter);
  ctx.r3.u64 = 0x80;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  sub_821A3320(ctx, base);
  uint32_t widget = ctx.r3.u32;
  if (!widget) return;
  REX_STORE_U32(g_scratch, LabelHash(row));
  ctx.r3.u64 = widget;
  ctx.r4.u64 = g_scratch;
  ctx.r5.u64 = kIdBase + row;
  ctx.r6.u64 = OnHash(row);
  ctx.r7.u64 = OffHash(row);
  ctx.r8.u64 = 1;
  sub_82345AE0(ctx, base);
  ctx.r3.u64 = screen;
  ctx.r4.u64 = name;
  ctx.r5.u64 = widget;
  sub_82313730(ctx, base);
}

void DrawValue(PPCContext& ctx, uint8_t* base, uint32_t widget, const Row& r) {
  uint32_t text = REX_LOAD_U32(widget + 0x1C);
  if (!text) return;
  ctx.r3.u64 = text;
  ctx.r4.u64 = REX_LOAD_U32(widget + (r.value ? 0x78 : 0x7C));
  sub_822FA300(ctx, base);
}

void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t offset) {
  uint32_t vtable = REX_LOAD_U32(object);
  uint32_t fn = REX_LOAD_U32(vtable + offset);
  if (PPCFunc* f = rex::runtime::ResolveIndirectFunction(fn)) {
    ctx.r3.u64 = object;
    f(ctx, base);
  }
}

}  // namespace

// AddOption (screen, id): ours after the screen's last row.
extern "C" REX_FUNC(sub_82347400) {
  const uint32_t screen = ctx.r3.u32, id = ctx.r4.u32;
  __imp__sub_82347400(ctx, base);
  if (REXCVAR_GET(nfscarbon_options_menu_log)) {
    REXLOG_INFO("[menu] row id 0x{:X}", id);
  }
  if (id != kAfterId || !REXCVAR_GET(nfscarbon_options_menu)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  LoadOnce();
  if (!EnsureGuestMemory(ctx, base)) return;
  for (uint32_t i = 0; i < kRowCount; ++i) AddRow(ctx, base, screen, i);
  REXLOG_INFO("[menu] {} row(s) added after 0x{:X}", kRowCount, id);
}

// On/off widget Draw.
extern "C" REX_FUNC(sub_82345C90) {
  const uint32_t widget = ctx.r3.u32;
  int row = RowOf(REX_LOAD_U32(widget + 0x74));
  if (row < 0) {
    __imp__sub_82345C90(ctx, base);
    return;
  }
  DrawValue(ctx, base, widget, g_rows[row]);
}

// On/off widget Act (r5 = the pad event).
extern "C" REX_FUNC(sub_82345B68) {
  const uint32_t widget = ctx.r3.u32, pad = ctx.r5.u32;
  int row = RowOf(REX_LOAD_U32(widget + 0x74));
  if (row < 0) {
    __imp__sub_82345B68(ctx, base);
    return;
  }
  if (!REX_LOAD_U8(widget + 0x64) || (pad != kPadLeft && pad != kPadRight)) return;
  Row& r = g_rows[row];
  r.value = !r.value;
  Apply(r);
  Save();
  REXLOG_INFO("[menu] {} = {}", r.key, r.value ? r.on : r.off);
  ctx.r4.u64 = pad;
  CallVirtual(ctx, base, widget, 0x38);  // what the game does after a change
  DrawValue(ctx, base, widget, r);
}

// On/off widget Reset ("restore defaults").
extern "C" REX_FUNC(sub_82345D38) {
  const uint32_t widget = ctx.r3.u32;
  int row = RowOf(REX_LOAD_U32(widget + 0x74));
  if (row < 0) {
    __imp__sub_82345D38(ctx, base);
    return;
  }
  Row& r = g_rows[row];
  if (r.value != r.dflt) {
    r.value = r.dflt;
    Apply(r);
    Save();
  }
  DrawValue(ctx, base, widget, r);
}

// Text lookup (hash -> string of the current language).
extern "C" REX_FUNC(sub_8231F8F8) {
  const uint32_t hash = ctx.r3.u32;
  if (!g_loaded.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadOnce();
  }
  if ((hash >> 8) == (kTextBase >> 8)) {
    if (uint32_t s = StringFor(hash)) {
      ctx.r3.u64 = s;
      return;
    }
  }
  __imp__sub_8231F8F8(ctx, base);
}
