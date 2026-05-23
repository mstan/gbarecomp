// gba_input.h — STUB. KEYINPUT / KEYCNT.
//
// 10-button matrix: A, B, Select, Start, Right, Left, Up, Down, R, L.
// KEYCNT can pend an IRQ on any key match (AND or OR semantics).
//
// Reference: GBATEK § "GBA Keypad Input".

#pragma once

namespace gba {

class GbaInput {
public:
    GbaInput();
    ~GbaInput();
};

}  // namespace gba
