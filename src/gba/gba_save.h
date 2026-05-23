// gba_save.h — STUB. Cartridge save chip controller.
//
// Detect on ROM scan: the standard heuristic is to grep the ROM for
// SRAM_Vnnn / FLASH_Vnnn / FLASH512_Vnnn / FLASH1M_Vnnn / EEPROM_Vnnn
// strings (Nintendo SDK signatures). Per-chip state machines:
//   - SRAM 32K: plain byte read/write at 0x0E000000.
//   - Flash 64K (512Kbit) / 128K (1Mbit): command-driven; manufacturer
//     IDs differ. Two banks for 128K (selected by command).
//   - EEPROM 512 byte / 8 KB: serial protocol over DMA from a tiny ROM
//     window at the top of the cartridge address space.
//
// Reference: GBATEK § "GBA Backup Memory".

#pragma once

namespace gba {

class GbaSave {
public:
    GbaSave();
    ~GbaSave();
};

}  // namespace gba
