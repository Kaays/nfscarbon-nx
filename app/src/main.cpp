// nfscarbon-nx - ReXGlue Recompiled Project (Need for Speed: Carbon, Nintendo Switch)

#include "generated/default/nfscarbon_init.h"

#include "nfscarbon_app.h"

REX_DEFINE_APP(nfscarbon, NfsCarbonApp::Create)

// Fallback for an edition built without options_menu_hook.cpp (its addresses not found yet):
// the hook's definition wins at link time.
__attribute__((weak)) void NfsCarbonLoadOptions() {}
