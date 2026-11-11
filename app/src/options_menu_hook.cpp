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
// does not have (kIdBase..), so those three are wrapped: for our ids pad left/right steps through
// the row's values (two or more: the widget only shows the text we set).
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
#include <cmath>
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
constexpr uint32_t kMaxValues = 4;

struct Value {
  const char* text;  // shown in the menu, and written in nfscarbon_options.txt
  const char* set;   // "cvar=value;cvar=value"
};
struct Row {
  const char* key;  // in nfscarbon_options.txt
  const char* label;
  Value values[kMaxValues];
  uint32_t count;
  uint32_t dflt;
  uint32_t value;
};
Row g_rows[] = {
    {"motion_blur", "Motion Blur",
     {{"On", "carbon_gpu_motion_blur=true"}, {"Off", "carbon_gpu_motion_blur=false"}}, 2, 0, 0},
    {"antialiasing", "Anti-Aliasing",
     {{"Off", "carbon_gpu_antialiasing=false"}, {"On", "carbon_gpu_antialiasing=true"}}, 2, 0, 0},
    // nfscarbon_lod_bias: the game's own detail, or small far objects dropping out sooner (fewer
    // draws in crowded races; the shipped toml has 0.6).
    {"scene_detail", "Scene Detail",
     {{"Full", "nfscarbon_lod_bias=1.0"}, {"Reduced", "nfscarbon_lod_bias=0.6"}}, 2, 1, 1},
    // The car reflection cube: the six faces every frame, one face a frame (each fresh every 6
    // frames), or none and only the disc's static cube (matte paint, as before v0.4.7; the
    // fewest draws).
    {"car_reflections", "Car Reflections",
     {{"Full", "carbon_gpu_reflection_faces=6;carbon_gpu_cube_from_faces=true"},
      {"Reduced", "carbon_gpu_reflection_faces=1;carbon_gpu_cube_from_faces=true"},
      {"Off", "carbon_gpu_reflection_faces=0;carbon_gpu_cube_from_faces=false"}},
     3, 0, 0},
    // The rear-view mirror (and the boss picture-in-picture box) every frame or every other one.
    {"mirror", "Rear-View Mirror",
     {{"Full Rate", "carbon_gpu_mirror_half_rate=false"}, {"Half Rate", "carbon_gpu_mirror_half_rate=true"}},
     2, 0, 0},
};
constexpr uint32_t kRowCount = sizeof(g_rows) / sizeof(g_rows[0]);

// Text hashes of our strings: per row its label, then its values.
constexpr uint32_t kTextBase = 0x7A5E0100u;
constexpr uint32_t kTextsPerRow = 1 + kMaxValues;
uint32_t TextHash(uint32_t row, uint32_t k) { return kTextBase + row * kTextsPerRow + k; }

std::mutex g_mutex;
uint32_t g_strings = 0;  // guest block: kTextsPerRow strings of kStringBytes per row
constexpr uint32_t kStringBytes = 48;
uint32_t g_scratch = 0;  // guest word for the constructor's label pointer
std::atomic<bool> g_loaded{false};

std::filesystem::path OptionsPath() {
  return rex::filesystem::GetExecutableFolder() / "nfscarbon_options.txt";
}

// Calls f(cvar, value) for each "cvar=value" of a value's settings.
template <typename F>
void ForEachSetting(const char* set, F f) {
  std::string all = set;
  size_t at = 0;
  while (at < all.size()) {
    size_t end = all.find(';', at);
    if (end == std::string::npos) end = all.size();
    std::string item = all.substr(at, end - at);
    size_t eq = item.find('=');
    if (eq != std::string::npos) f(item.substr(0, eq), item.substr(eq + 1));
    at = end + 1;
  }
}

void Apply(const Row& r) {
  ForEachSetting(r.values[r.value].set, [](const std::string& cvar, const std::string& v) {
    rex::cvar::SetFlagByName(cvar, v);
  });
}

bool SameSetting(const std::string& now, const std::string& want) {
  if (now == want) return true;
  char* e1 = nullptr;
  char* e2 = nullptr;
  double a = std::strtod(now.c_str(), &e1), b = std::strtod(want.c_str(), &e2);
  return e1 != now.c_str() && e2 != want.c_str() && std::abs(a - b) < 1e-3;
}

void Save() {
  std::ofstream f(OptionsPath(), std::ios::trunc);
  if (!f) {
    REXLOG_WARN("[menu] cannot write {}", OptionsPath().string());
    return;
  }
  f << "# nfscarbon-nx: settings changed in the game's Options screen (they win over nfscarbon.toml)\n";
  for (const Row& r : g_rows) f << r.key << "=" << r.values[r.value].text << "\n";
}

// Rows start from the value matching the cvars (the toml), then the file overrides them.
void LoadOnce() {
  if (g_loaded.exchange(true)) return;
  for (Row& r : g_rows) {
    r.value = r.dflt;
    for (uint32_t k = 0; k < r.count; ++k) {
      bool all = true;
      ForEachSetting(r.values[k].set, [&](const std::string& cvar, const std::string& v) {
        all = all && SameSetting(rex::cvar::GetFlagByName(cvar), v);
      });
      if (all) {
        r.value = k;
        break;
      }
    }
  }
  std::ifstream f(OptionsPath());
  std::string line;
  uint32_t n = 0;
  while (f && std::getline(f, line)) {
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    for (Row& r : g_rows) {
      if (k != r.key) continue;
      int pick = -1;
      for (uint32_t i = 0; i < r.count; ++i) {
        if (v == r.values[i].text) pick = int(i);
      }
      // Older files: 1 = the first value, 0 = the second.
      if (pick < 0 && (v == "1" || v == "0")) pick = v == "1" ? 0 : 1;
      if (pick < 0) continue;
      r.value = uint32_t(pick);
      Apply(r);
      ++n;
    }
  }
  REXLOG_INFO("[menu] {} setting(s) from {}", n, OptionsPath().string());
}

int RowOf(uint32_t id) {
  return id >= kIdBase && id < kIdBase + kRowCount ? int(id - kIdBase) : -1;
}

void PutString(uint8_t* base, uint32_t at, const char* s) {
  uint32_t i = 0;
  for (; s && s[i] && i + 1 < kStringBytes; ++i) REX_STORE_U8(at + i, uint8_t(s[i]));
  REX_STORE_U8(at + i, 0);
}

// Guest memory for our strings and the scratch word, taken once from the game's allocator.
bool EnsureGuestMemory(PPCContext& ctx, uint8_t* base) {
  if (g_strings) return true;
  ctx.r3.u64 = kRowCount * kTextsPerRow * kStringBytes + 16;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  sub_821A3320(ctx, base);
  uint32_t block = ctx.r3.u32;
  if (!block) return false;
  for (uint32_t i = 0; i < kRowCount; ++i) {
    PutString(base, block + 16 + (i * kTextsPerRow) * kStringBytes, g_rows[i].label);
    for (uint32_t k = 0; k < kMaxValues; ++k) {
      PutString(base, block + 16 + (i * kTextsPerRow + 1 + k) * kStringBytes,
                k < g_rows[i].count ? g_rows[i].values[k].text : "");
    }
  }
  g_scratch = block;
  g_strings = block + 16;
  return true;
}

uint32_t StringFor(uint32_t hash) {
  if (!g_strings || hash < kTextBase || hash >= kTextBase + kRowCount * kTextsPerRow) return 0;
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
  REX_STORE_U32(g_scratch, TextHash(row, 0));
  ctx.r3.u64 = widget;
  ctx.r4.u64 = g_scratch;
  ctx.r5.u64 = kIdBase + row;
  ctx.r6.u64 = TextHash(row, 1);
  ctx.r7.u64 = TextHash(row, 2);
  ctx.r8.u64 = 1;
  sub_82345AE0(ctx, base);
  ctx.r3.u64 = screen;
  ctx.r4.u64 = name;
  ctx.r5.u64 = widget;
  sub_82313730(ctx, base);
}

void DrawValue(PPCContext& ctx, uint8_t* base, uint32_t widget, uint32_t row) {
  uint32_t text = REX_LOAD_U32(widget + 0x1C);
  if (!text) return;
  ctx.r3.u64 = text;
  ctx.r4.u64 = TextHash(row, 1 + g_rows[row].value);
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
  DrawValue(ctx, base, widget, uint32_t(row));
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
  r.value = pad == kPadRight ? (r.value + 1) % r.count : (r.value + r.count - 1) % r.count;
  Apply(r);
  Save();
  REXLOG_INFO("[menu] {} = {}", r.key, r.values[r.value].text);
  ctx.r4.u64 = pad;
  CallVirtual(ctx, base, widget, 0x38);  // what the game does after a change
  DrawValue(ctx, base, widget, uint32_t(row));
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
  DrawValue(ctx, base, widget, uint32_t(row));
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
