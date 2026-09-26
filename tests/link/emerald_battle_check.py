"""Assert a native two-player Emerald battle KO or persisted battle result.

The default checks a combat turn (Salamence against the traded Torchic).
--result instead checks complementary win/loss records saved after battle exit.
"""
import argparse
import struct
from emerald_trade_check import read_session, party_identities, flash_report


def battle(path):
    identity, cycle, devices = read_session(path)
    masters = []
    parties = []
    for seat, device in enumerate(devices):
        flags, = struct.unpack_from("<I", device, 0x22FEC)
        if flags & 0x2A != 0x2A or device[0x2406C] != 2 or device[0x2433A] != 0:
            raise ValueError("expected an ongoing two-battler trainer link battle")
        iwram = device[0x40000:0x48000]
        if iwram[0x3171:0x3174] != bytes((4, seat, 2)) or iwram[0x3124] != 1:
            raise ValueError("both cable players must remain connected")
        count = device[0x244E9]
        party = party_identities(count, device[0x244EC:0x244EC + 600])
        parties.append(dict((mon, struct.unpack_from("<H", device, 0x244EC + slot * 100 + 86)[0])
                            for slot, mon in enumerate(party)))
        # Emerald's cable battle master computes combat. The other cartridge
        # runs its link battle controller; its gBattleMons is not a second
        # authoritative simulation of combat (it may be zero).
        if flags & 4:
            mons = []
            for index in range(2):
                record = device[0x24084 + index * 88:0x24084 + (index + 1) * 88]
                pid, = struct.unpack_from("<I", record, 0x48)
                ot, = struct.unpack_from("<I", record, 0x54)
                hp, = struct.unpack_from("<H", record, 0x28)
                maximum, = struct.unpack_from("<H", record, 0x2C)
                mons.append(((pid, ot), hp, maximum, sum(record[0x24:0x28])))
            masters.append(mons)
    if len(masters) != 1:
        raise ValueError("expected exactly one Emerald battle master")
    return identity, cycle, masters[0], parties


def check_battle(before_path, after_path):
    identity, start, before, parties_before = battle(before_path)
    other_identity, end, after, parties_after = battle(after_path)
    if identity != other_identity or end <= start:
        raise ValueError("snapshots must describe the same advancing session")
    if [mon[0] for mon in before] != [mon[0] for mon in after]:
        raise ValueError("checker requires the same two active Pokemon")
    if not all(0 < mon[1] <= mon[2] for mon in before):
        raise ValueError("both Pokemon must start conscious")
    knockouts = [i for i in range(2) if after[i][1] == 0 and after[i][2] == before[i][2]]
    if len(knockouts) != 1:
        raise ValueError("expected exactly one knockout")
    fainted = after[knockouts[0]][0]
    if not any(before[i][3] > after[i][3] for i in range(2)):
        raise ValueError("no move PP was consumed")
    if not any(fainted in party and party[fainted] > 0 for party in parties_before):
        raise ValueError("fainted Pokemon was not initially in a player's party")
    if not any(fainted in party and party[fainted] == 0 for party in parties_after):
        raise ValueError("owner's cartridge did not receive the knockout")
    print(f"native Emerald cable battle KO verified across {end - start} cycles; "
          "move PP consumed, owner party updated, both links healthy")


def check_result(before_path, after_path):
    identity, start, before = read_session(before_path)
    other_identity, end, after = read_session(after_path)
    if identity != other_identity or end <= start:
        raise ValueError("snapshots must describe the same advancing session")
    outcomes = []
    for seat, (old, new) in enumerate(zip(before, after)):
        old_flags, = struct.unpack_from("<I", old, 0x22FEC)
        new_flags, = struct.unpack_from("<I", new, 0x22FEC)
        outcome = new[0x2433A]
        if old_flags & 0x22 != 0x22 or new_flags & 0x20 or outcome not in (1, 2):
            raise ValueError("expected an active cable battle followed by a win/loss result")
        stats = []
        for device in (old, new):
            sb1, sb2 = struct.unpack_from("<II", device, 0x40000 + 0x5D8C)
            sb1 -= 0x02000000
            sb2 -= 0x02000000
            if not 0 <= sb1 <= 0x40000 - 0x1620 or not 0 <= sb2 <= 0x40000 - 0xB0:
                raise ValueError("invalid Emerald save-block pointers")
            key, = struct.unpack_from("<I", device, sb2 + 0xAC)
            stats.append(tuple(value ^ key for value in struct.unpack_from("<3I", device, sb1 + 0x159C + 23 * 4)))
        expected = list(stats[0])
        expected[outcome - 1] += 1
        saved = flash_report(new)
        if stats[1] != tuple(expected) or saved.battle_stats != stats[1]:
            raise ValueError(f"machine {seat}: battle result was not recorded in RAM and the newest save")
        if saved.digest == flash_report(old).digest:
            raise ValueError("post-battle flash did not change")
        party = party_identities(new[0x244E9], new[0x244EC:0x244EC + 600])
        if party != saved.party:
            raise ValueError("saved party does not match after battle exit")
        outcomes.append(outcome)
        print(f"machine {seat}: {'win' if outcome == 1 else 'loss'} persisted; "
              f"wins/losses/draws {saved.battle_stats}, 28 valid save sectors")
    if sorted(outcomes) != [1, 2]:
        raise ValueError("battle outcomes are not complementary")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("before")
    p.add_argument("after")
    p.add_argument("--result", action="store_true", help="check complementary persisted win/loss after battle exit")
    args = p.parse_args()
    (check_result if args.result else check_battle)(args.before, args.after)
