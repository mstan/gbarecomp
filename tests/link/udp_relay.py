"""Bounded external latency/loss injection shared by native link harnesses."""
import heapq
import random
import select
import socket
import struct
import threading
import time


def reserve_routes():
    """Reserve two peer endpoints and retain the two corresponding UDP hops."""
    for _ in range(100):
        port = random.randrange(20000, 55000)
        sockets = [socket.socket(type=socket.SOCK_DGRAM) for _ in range(4)]
        try:
            for i, sock in enumerate(sockets):
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
                sock.bind(("127.0.0.1", port+i))
                sock.setblocking(False)
            return port, sockets[2:]
        except OSError:
            for sock in sockets:
                sock.close()
        finally:
            for sock in sockets[:2]:
                sock.close()
    raise RuntimeError("no free loopback port pair")


class UdpRelay:
    def __init__(self, port, sockets, mode="ordinary", outage_trigger=None):
        self.port, self.sockets, self.mode = port, sockets, mode
        self.outage_trigger = outage_trigger
        self.outage_started = None
        self.dropped = self.delayed = self.peak = 0
        self.upload_acks_dropped = self.ready_acks_dropped = 0
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

    def _run(self):
        pending = []
        rng = [random.Random(17), random.Random(18)]
        try:
            while not self.stop.is_set():
                if self.outage_started is None and self.outage_trigger and self.outage_trigger.exists():
                    self.outage_started = time.monotonic()
                now = time.monotonic()
                while pending and pending[0][0] <= now:
                    _, _, sender, packet = heapq.heappop(pending)
                    if self._outage():
                        self.dropped += 1
                    else:
                        self.sockets[1-sender].sendto(packet, ("127.0.0.1", self.port+1-sender))
                timeout = min(0.005, max(0,pending[0][0]-now)) if pending else 0.005
                ready, _, _ = select.select(self.sockets, [], [], timeout)
                for sock in ready:
                    try:
                        packet, _ = sock.recvfrom(65535)
                    except (ConnectionResetError, BlockingIOError):
                        # Windows may report an early UDP send to the peer
                        # before it binds as ICMP on recv. HELLO retries.
                        continue
                    if self._outage():
                        self.dropped += 1
                        continue
                    sender = self.sockets.index(sock)
                    if self.mode == "startup_loss" and len(packet) >= 24:
                        # Pinned recomp-net wire: type u16 at 4, xfer id u32
                        # at 14. BOOT must implicitly ACK an uploaded save;
                        # the ready barrier must retransmit lost responses.
                        kind = struct.unpack_from("<H",packet,4)[0]
                        transfer = struct.unpack_from("<I",packet,14)[0]
                        if sender == 0 and kind == 10 and transfer == 0x40000001:
                            self.upload_acks_dropped += 1
                            continue
                        if sender == 1 and kind == 12 and self.ready_acks_dropped < 3:
                            self.ready_acks_dropped += 1
                            continue
                    if self.mode == "loss" and rng[sender].randrange(100) < 2:
                        self.dropped += 1
                        continue
                    if len(pending) >= 8192:
                        raise RuntimeError("UDP simulation queue overflow; latency result invalid")
                    self.delayed += 1
                    heapq.heappush(pending,(time.monotonic()+rng[sender].uniform(0.030,0.050),
                                           self.delayed,sender,packet))
                    self.peak = max(self.peak,len(pending))
        except Exception as exc:
            self.errors.append(exc)
