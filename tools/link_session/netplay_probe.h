#pragma once
#include "multiplayer_session.h"
#include <functional>

// Test-only native cartridge runner. Uses the same session/host/barriers as the
// synthetic fixture; does not implement a separate serial or network protocol.
void gba_link_probe_netplay(gbarecomp::GbaMultiplayerSession&, unsigned frames,
    const std::function<std::array<std::uint16_t,2>(std::uint32_t)>& input,
    const std::string& identity);
