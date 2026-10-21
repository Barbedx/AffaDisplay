// tools/host_main.cpp — NOT part of the library.
//
// env:native exists so the whole library can be compiled for the host with
// -Wall -Wextra -Wundef and driven by `pio test -e native`. A test build supplies its own
// main() (each suite ends in `int main(int, char**) { UNITY_BEGIN(); ... }`), but a plain
// `pio run` of the same environment had no entry point and the linker failed with
// `undefined reference to WinMain`. That made the obvious whole-project command — `pio run`
// with no -e — report a failure that had nothing to do with the code.
//
// So: this translation unit provides an entry point ONLY when the environment is linked as
// a program rather than as a test. PlatformIO defines UNIT_TEST (and, since 6.x,
// PIO_UNIT_TESTING) for test builds; under either one this file compiles to nothing and
// Unity's main() is the only one in the image.
//
// It is pulled in by build_src_filter in [env:native] alone, so it never reaches a target
// build and never reaches a consumer who added AffaDisplay to lib_deps.

#if !defined(UNIT_TEST) && !defined(PIO_UNIT_TESTING)

#include "AffaDisplay.h"

namespace {
struct HostClock final : affa::IClock {
  uint32_t millis() const override { return _ms; }
  uint32_t _ms = 0;
};
}  // namespace

// Linking is the point: drive a real panel through begin() and one poll() so that a host
// `pio run` is a genuine link check of the selected gates, not an empty main.
int main(int, char**) {
  affa::LoopbackLink<16> link;
  HostClock              clock;

#if AFFA_PANEL_CARMINAT
  affa::CarminatDisplay display(link, clock);
#elif AFFA_PANEL_UPDATELIST
  affa::UpdateListDisplay display(link, clock);
#else
  // No panel selected: the core still has to link.
  return affa::expired(0, 0) ? 0 : 1;
#endif

#if AFFA_PANEL_CARMINAT || AFFA_PANEL_UPDATELIST
  if (!display.begin()) return 1;
  display.poll();
  return 0;
#endif
}

#endif  // !UNIT_TEST && !PIO_UNIT_TESTING
