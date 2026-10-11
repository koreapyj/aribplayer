#include "TestHost.h"

#include <cstdlib>

namespace wvptest {

void HostInit() {
    // Must be set before libffplay/SDL initialise.
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);
}

}
