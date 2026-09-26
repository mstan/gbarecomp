"""Read-only USA Emerald trade assertions for two GBSS v1 probe snapshots.

Supply snapshots from before selecting the offered Pokemon and after the trade
has saved. No ROM, Pokemon data, or save image is written by this checker.
"""
import argparse
import collections
import hashlib
import pathlib
import struct

ROM_SHA1 = b"f3ae088181bf583e55daf962a92bb46f4f1d07b7"
SECTOR_SIZES = [0xF2C, 0xF80, 0xF80, 0xF80, 0xF08] + [0xF80] * 8 + [0x7D0]


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, count):
        if count < 0 or self.pos + count > len(self.data):
            raise ValueError("truncated paired snapshot")
        result = self.data[self.pos:self.pos + count]
        self.pos += count
        return result

    def unpack(self, fmt):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))

    def count(self, maximum):
        count, = self.unpack("I")
        if count > maximum:
            raise ValueError("snapshot count exceeds qualification bound")
        return count

    def blob(self, maximum):
        return self.take(self.count(maximum))


def party_identities(count, records):
    if not 1 <= count <= 6 or len(records) < count * 100:
        raise ValueError("invalid Emerald party")
    party = []
    for slot in range(count):
        mon = records[slot * 100:(slot + 1) * 100]
        personality, trainer = struct.unpack_from("<II", mon)
        decoded = struct.pack("<12I", *(
            word ^ personality ^ trainer for word in struct.unpack_from("<12I", mon, 32)))
        if sum(struct.unpack("<24H", decoded)) & 0xFFFF != struct.unpack_from("<H", mon, 28)[0]:
            raise ValueError("party Pokemon checksum mismatch")
        if mon[19] & 1:
            raise ValueError("party contains a bad egg")
        party.append((personality, trainer))
    return party


def flash_report(device):
    # The device archive is private to the runtime. Locate a *complete* Emerald
    # Flash1M vector by its length and all 28 sector signatures, then validate
    # its content. Refuse ambiguity or an in-flight erase/program operation.
    # This is an asset-specific diagnostic, never a production state decoder.
    candidates = []
    pos = 0
    while True:
        pos = device.find(struct.pack("<I", 131072), pos)
        if pos < 0:
            break
        pos += 4
        image = device[pos:pos + 131072]
        if len(image) == 131072 and all(
            struct.unpack_from("<I", image, j * 4096 + 0xFF8)[0] == 0x08012025
            for j in range(28)
        ):
            candidates.append(image)
    if len(candidates) != 1:
        raise ValueError("expected one complete Emerald Flash1M image")
    image = candidates[0]
    banks, saved_parties = [], []
    for bank in range(2):
        ids, counters, sections = set(), set(), {}
        for j in range(bank * 14, (bank + 1) * 14):
            sector = image[j * 4096:(j + 1) * 4096]
            sid, checksum, _, counter = struct.unpack_from("<HHII", sector, 0xFF4)
            if sid >= 14 or sid in ids:
                raise ValueError("invalid or duplicate Emerald save sector")
            ids.add(sid)
            counters.add(counter)
            payload = sector[:SECTOR_SIZES[sid]]
            sections[sid] = payload
            total = sum(struct.unpack("<" + "I" * (len(payload) // 4), payload)) & 0xFFFFFFFF
            if ((total & 0xFFFF) + (total >> 16)) & 0xFFFF != checksum:
                raise ValueError("Emerald save sector checksum mismatch")
        if len(counters) != 1:
            raise ValueError("save bank contains mixed generations")
        banks.append(counters.pop())
        saveblock = b"".join(sections[i] for i in range(1, 5))
        saved_parties.append(party_identities(saveblock[0x234], saveblock[0x238:0x238 + 600]))
    # Emerald's counter is u32; respect wrap when comparing its two complete banks.
    newest = int(0 < ((banks[1] - banks[0]) & 0xFFFFFFFF) < 0x80000000)
    return hashlib.sha256(image).hexdigest(), banks, saved_parties[newest]


def read_snapshot(path):
    with pathlib.Path(path).open("rb") as stream:
        data = stream.read(8 * 1024 * 1024 + 1)
    if len(data) > 8 * 1024 * 1024:
        raise ValueError("paired snapshot exceeds bound")
    r = Reader(data)
    if r.unpack("III") != (0x53534247, 1, 2):
        raise ValueError("expected a two-machine GBSS v1 snapshot")
    manifest = []
    for _ in range(2):
        mid, = r.unpack("I")
        program, rom = r.blob(1024), r.blob(1024)
        boot, = r.unpack("B")
        if rom != ROM_SHA1 or boot != 0:
            raise ValueError("checker requires native USA Emerald cartridges")
        manifest.append((mid, program, rom, boot))
    if [m[0] for m in manifest] != [0, 1]:
        raise ValueError("checker requires probe machine order 0, 1")
    inputs = r.take(4 * r.count(8))
    links = []
    for _ in range(r.count(8)):
        medium = r.take(1)
        links.append((medium, r.take(4 * r.count(8))))
    cycle, = r.unpack("Q")
    machines = []
    for _ in range(2):
        r.take(180)  # canonical CPU registers, no host pointers
        r.take(4 * r.count(1024))  # native return PCs
        r.take(32 * r.count(1024))  # nested IRQ frames
        r.take(20 + 16)  # continuation and device timing
        device = r.blob(8 * 1024 * 1024)
        if len(device) < 0x48000:
            raise ValueError("missing canonical EWRAM/IWRAM")
        party_count = device[0x244E9]  # gPlayerPartyCount, pinned USA symbols
        if not 2 <= party_count <= 6:
            raise ValueError("trade party must contain two to six Pokemon")
        party = party_identities(party_count, device[0x244EC:0x244EC + 600])
        flash_hash, generations, saved_party = flash_report(device)
        # gLink hardwareError / badChecksum / queueFull / lag and global error.
        iwram = device[0x40000:0x48000]
        if any(iwram[0x3180:0x3184]) or iwram[0x306C]:
            raise ValueError("game reports a cable error")
        machines.append((party, flash_hash, generations, saved_party))
    r.blob(4096)
    if r.pos != len(data):
        raise ValueError("trailing paired snapshot bytes")
    return (manifest, inputs, links), cycle, machines


def check_trade(before_path, after_path, slots=(0, 0)):
    identity, start, before = read_snapshot(before_path)
    other_identity, end, after = read_snapshot(after_path)
    if identity != other_identity or end <= start:
        raise ValueError("snapshots must describe the same advancing session")
    selected = []
    for seat, slot in enumerate(slots):
        if not 0 <= slot < len(before[seat][0]):
            raise ValueError("offered party slot is outside the party")
        selected.append(before[seat][0][slot])
    combined = before[0][0] + before[1][0]
    if any(combined.count(mon) != 1 for mon in selected):
        raise ValueError("offered Pokemon identities must be distinct and unique")
    for seat, slot in enumerate(slots):
        expected = list(before[seat][0])
        expected[slot] = selected[1 - seat]
        if collections.Counter(after[seat][0]) != collections.Counter(expected):
            raise ValueError(f"machine {seat}: expected reciprocal party exchange did not occur")
        if before[seat][1] == after[seat][1]:
            raise ValueError(f"machine {seat}: flash did not change after the trade")
        if after[seat][0] != after[seat][3]:
            raise ValueError(f"machine {seat}: newest complete save does not contain the current party")
        print(f"machine {seat}: reciprocal trade persisted; party and 28 sector checksums valid; "
              f"save generations {after[seat][2]}")
    print(f"paired native Emerald trade verified across {end - start} emulated cycles")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("before")
    p.add_argument("after")
    p.add_argument("--slots", nargs=2, type=int, default=[0, 0], metavar=("P0", "P1"))
    args = p.parse_args()
    check_trade(args.before, args.after, args.slots)


if __name__ == "__main__":
    main()
