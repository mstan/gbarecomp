"""Bounded external latency/loss injection shared by native link harnesses.

Every peer i binds port+i and dials its own proxy socket at port+N+i, so each
peer only ever talks to one fixed endpoint. Two topologies:

  sfu  (default) like the lobby server's UDP input relay: a packet from peer i
       fans out to every other peer, each copy leaving that peer's own proxy.
  hub  a LAN star (rnet_session_start_lan_hub on seat 0): guests' packets go
       only to seat 0, which learns each guest at that guest's proxy address;
       seat 0's replies to a proxy address are delivered to that guest.

With two peers both topologies are the original two-socket relay.
"""
import heapq
import random
import select
import socket
import struct
import threading
import time


def reserve_routes(players=2):
    """Reserve N peer endpoints and retain the N corresponding UDP hops."""
    for _ in range(100):
        port = random.randrange(20000, 55000 - 2 * players)
        sockets = [socket.socket(type=socket.SOCK_DGRAM) for _ in range(2 * players)]
        try:
            for i, sock in enumerate(sockets):
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
                sock.bind(("127.0.0.1", port+i))
                sock.setblocking(False)
            return port, sockets[players:]
        except OSError:
            for sock in sockets:
                sock.close()
        finally:
            for sock in sockets[:players]:
                sock.close()
    raise RuntimeError("no free loopback port range")


class UdpRelay:
    def __init__(self, port, sockets, mode="ordinary", outage_trigger=None, topology="sfu"):
        self.port, self.sockets, self.mode = port, sockets, mode
        self.players = len(sockets)
        if topology not in ("sfu", "hub"):
            raise ValueError("relay topology must be sfu or hub")
        self.topology = topology
        self.outage_trigger = outage_trigger
        self.outage_started = None
        self.dropped = self.delayed = self.peak = 0
        self.upload_acks_dropped = self.ready_acks_dropped = 0
        self.commit_hole_dropped = self.commit_hole_requested = 0
        self.errors = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=5)
        for sock in self.sockets:
            sock.close()

    def _outage(self):
        return self.outage_started is not None and time.monotonic()-self.outage_started < 6

    def _routes(self, proxy, source):
        """(sender seat, [destination seats]) for a datagram on proxy socket."""
        if source == self.port + proxy:  # peer `proxy` dialing its own hop
            if self.topology == "hub" and proxy != 0:
                return proxy, [0]
            return proxy, [j for j in range(self.players) if j != proxy]
        if self.topology == "hub" and source == self.port:  # hub answering a guest
            return 0, [proxy]
        return None, []

    def _run(self):
        pending = []
        rng = [random.Random(17 + i) for i in range(self.players)]
        try:
            while not self.stop.is_set():
                if self.outage_started is None and self.outage_trigger and self.outage_trigger.exists():
                    self.outage_started = time.monotonic()
                now = time.monotonic()
                while pending and pending[0][0] <= now:
                    _, _, via, dest, packet = heapq.heappop(pending)
                    if self._outage():
                        self.dropped += 1
                    else:
                        self.sockets[via].sendto(packet, ("127.0.0.1", self.port+dest))
                timeout = min(0.005, max(0,pending[0][0]-now)) if pending else 0.005
                ready, _, _ = select.select(self.sockets, [], [], timeout)
                for sock in ready:
                    try:
                        packet, source = sock.recvfrom(65535)
                    except (ConnectionResetError, BlockingIOError):
                        # Windows may report an early UDP send to the peer
                        # before it binds as ICMP on recv. HELLO retries.
                        continue
                    if self._outage():
                        self.dropped += 1
                        continue
                    proxy = self.sockets.index(sock)
                    sender, destinations = self._routes(proxy, source[1])
                    if sender is None:
                        continue
                    if self.mode == "startup_loss" and len(packet) >= 24:
                        # Pinned recomp-net wire: type u16 at 4, xfer id u32
                        # at 14. The host's assembled startup broadcast must
                        # implicitly ACK each uploaded save (MEMCARD ids are
                        # 0x40000000 | (seat-1)<<24 | serial); the ready
                        # barrier must retransmit lost responses.
                        kind = struct.unpack_from("<H",packet,4)[0]
                        transfer = struct.unpack_from("<I",packet,14)[0]
                        if sender == 0 and kind == 10 and (transfer & 0xc0ffffff) == 0x40000001:
                            self.upload_acks_dropped += 1
                            continue
                        if sender != 0 and kind == 12 and self.ready_acks_dropped < 3:
                            self.ready_acks_dropped += 1
                            continue
                    if self.mode == "loss" and rng[sender].randrange(100) < 2:
                        self.dropped += 1
                        continue
                    for dest in destinations:
                        if self.mode == "commit_hole" and len(packet) >= 22:
                            kind = struct.unpack_from("<H",packet,4)[0]
                            if sender == 1 and kind == 20 and packet[12] == 6 and packet[11] == 0 and struct.unpack_from("<I",packet,18)[0] == 58:
                                self.commit_hole_requested += 1
                            if sender == 0 and dest == 1 and kind == 24 and struct.unpack_from("<I",packet,12)[0] == 58 and not self.commit_hole_requested:
                                self.commit_hole_dropped += 1
                                self.dropped += 1
                                continue
                        if len(pending) >= 8192:
                            raise RuntimeError("UDP simulation queue overflow; latency result invalid")
                        self.delayed += 1
                        # SFU copies leave the destination's own hop; a hub
                        # exchange stays on the guest's hop in both directions.
                        via = dest if self.topology == "sfu" else proxy
                        heapq.heappush(pending,(time.monotonic()+rng[sender].uniform(0.030,0.050),
                                               self.delayed,via,dest,packet))
                    self.peak = max(self.peak,len(pending))
        except Exception as exc:
            self.errors.append(exc)
