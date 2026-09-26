#pragma once
#include "runtime_arm.h"
namespace gba { class GbaBus; }
namespace gbarecomp {
// Classify only; never execute or interpret guest semantics. Unknown/device
// accesses terminate a native slice before the instruction's effects begin.
bool multiplayer_slice_safe(const gba::GbaBus&,const ArmCpuState&);
}
