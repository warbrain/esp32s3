"""Relay local voice UDP/Opus frames over a paired ESP32-S3 BLE connection.

Install with ``python -m pip install -r voice/tools/requirements-ble.txt``.
Run this script, then start voice with ``--ble``.
Use ``--device ADDRESS_OR_NAME`` when more than one voice board advertises.
Ctrl+C stops capture/playback and disconnects. No Wi-Fi connection is required.

Bleak API references https://bleak.readthedocs.io/en/latest/api/client.html
and https://bleak.readthedocs.io/en/latest/api/scanner.html.
"""

from __future__ import annotations

import argparse
import asyncio
from collections import deque
from contextlib import suppress
import logging
import math
import struct
import sys
import time


LOG = logging.getLogger("ble_voice_bridge")
SERVICE_UUID = "55c3a500-8ce7-4a82-a587-d4a23a7de738"
RX_UUID = "55c3a501-8ce7-4a82-a587-d4a23a7de738"
TX_UUID = "55c3a502-8ce7-4a82-a587-d4a23a7de738"
FIRST = 1
LAST = 2
HEADER = struct.Struct("<2sBBHHI")
MAX_PACKET_BYTES = 1612
MAX_GATT_VALUE_BYTES = 244
HELLO = 1
CONTROL = 4
AUDIO_TYPES = (2, 3)
AUDIO_DOWN = 3
BACKEND_HELLO = b"backend_hello"
BACKEND_GOODBYE = b"backend_goodbye"
LINK_READY = b"link_ready"
LINK_LOST = b"link_lost"
VOLUME_REQUEST = b"volume_request"
STOP_PLAYBACK = b"stop_playback"
MAX_AUDIO_AGE = 0.5
PEER_TIMEOUT = 10.0


def valid_packet(packet: bytes | bytearray) -> bool:
    return (
        HEADER.size <= len(packet) <= MAX_PACKET_BYTES
        and packet[:2] == b"\xa5\xc3"
        and 1 <= packet[2] <= 5
        and HEADER.size + struct.unpack_from("<H", packet, 6)[0] == len(packet)
    )


def make_packet(kind: int, payload: bytes, sequence: int = 0) -> bytes:
    if not 1 <= kind <= 5 or len(payload) > MAX_PACKET_BYTES - HEADER.size:
        raise ValueError("Invalid voice frame type or payload length")
    timestamp = int(time.monotonic() * 1000) & 0xFFFFFFFF
    return HEADER.pack(b"\xa5\xc3", kind, 0, sequence & 0xFFFF, len(payload), timestamp) + payload


def control_payload(packet: bytes) -> bytes | None:
    return packet[HEADER.size:] if packet[2] == CONTROL else None


def is_audio(packet: bytes | tuple[bytes, ...]) -> bool:
    return isinstance(packet, bytes) and packet[2] in AUDIO_TYPES


def fragments(packet: bytes, value_size: int):
    """value_size includes the single fragment flag byte."""
    if not valid_packet(packet) or not 2 <= value_size <= MAX_GATT_VALUE_BYTES:
        raise ValueError("Invalid packet or GATT value size")
    payload_size = value_size - 1
    for offset in range(0, len(packet), payload_size):
        end = min(offset + payload_size, len(packet))
        flags = (FIRST if offset == 0 else 0) | (LAST if end == len(packet) else 0)
        yield bytes((flags,)) + packet[offset:end]


class FrameAssembler:
    def __init__(self):
        self.buffer = bytearray()
        self.started = 0.0
        self.invalid = 0

    def reset(self):
        self.buffer.clear()
        self.started = 0.0

    def feed(self, fragment: bytes | bytearray) -> bytes | None:
        now = time.monotonic()
        if len(fragment) < 2 or len(fragment) > MAX_GATT_VALUE_BYTES or fragment[0] & ~(FIRST | LAST):
            self.invalid += 1
            self.reset()
            return None
        flags = fragment[0]
        if flags & FIRST:
            self.reset()
            self.started = now
        elif not self.buffer or now - self.started > 1.0:
            self.invalid += 1
            self.reset()
            return None
        self.buffer.extend(fragment[1:])
        if len(self.buffer) > MAX_PACKET_BYTES or (
            len(self.buffer) >= HEADER.size
            and (
                self.buffer[:2] != b"\xa5\xc3"
                or not 1 <= self.buffer[2] <= 5
                or HEADER.size + struct.unpack_from("<H", self.buffer, 6)[0] > MAX_PACKET_BYTES
                or len(self.buffer) > HEADER.size + struct.unpack_from("<H", self.buffer, 6)[0]
            )
        ):
            self.invalid += 1
            self.reset()
            return None
        if not flags & LAST:
            return None
        packet = bytes(self.buffer)
        self.reset()
        if not valid_packet(packet):
            self.invalid += 1
            return None
        return packet


class PacketQueue:
    """Keep order, shedding old audio first. A reset batch occupies one slot."""

    def __init__(self, limit: int):
        self.limit = limit
        self.packets = deque()
        self.available = asyncio.Event()
        self.dropped = 0

    def clear(self):
        self.packets.clear()
        self.available.clear()

    def discard_playback(self):
        retained = deque(
            (packet, queued_at) for packet, queued_at in self.packets
            if not isinstance(packet, bytes) or packet[2] != AUDIO_DOWN
        )
        self.dropped += len(self.packets) - len(retained)
        self.packets = retained
        if not retained:
            self.available.clear()

    def put(self, packet: bytes | tuple[bytes, ...], *, shed_audio: bool = True) -> bool:
        if len(self.packets) >= self.limit:
            if not shed_audio:
                self.dropped += 1
                return False
            for index, (queued, _) in enumerate(self.packets):
                if is_audio(queued):
                    del self.packets[index]
                    self.dropped += 1
                    break
            else:
                self.dropped += 1
                return False
        self.packets.append((packet, time.monotonic()))
        self.available.set()
        return True

    async def get(self) -> bytes | tuple[bytes, ...]:
        while True:
            await self.available.wait()
            if not self.packets:
                continue
            packet, queued_at = self.packets.popleft()
            if not self.packets:
                self.available.clear()
            if is_audio(packet) and time.monotonic() - queued_at > MAX_AUDIO_AGE:
                self.dropped += 1
                continue
            return packet


class UdpBridge(asyncio.DatagramProtocol):
    def __init__(self, queue_limit: int):
        self.to_board = PacketQueue(queue_limit)
        self.to_backend = PacketQueue(queue_limit)
        self.assembler = FrameAssembler()
        self.transport = None
        self.peer = None
        self.peer_seen = 0.0
        self.peer_epoch = 0
        self.peer_volume: bytes | None = None
        self.session = None
        self.session_ended = asyncio.Event()
        self.sent_ble = 0
        self.sent_udp = 0
        self.invalid_udp = 0

    def connection_made(self, transport):
        self.transport = transport

    def reset(self):
        self.session = None
        self.to_board.clear()
        self.to_backend.clear()
        self.assembler.reset()

    def inform_peer(self, state: bytes):
        if self.peer is not None and self.transport is not None:
            self.transport.sendto(make_packet(CONTROL, state), self.peer)

    def end_session(self, session):
        if session is self.session:
            self.inform_peer(LINK_LOST)
            self.reset()

    def reset_board_audio(self):
        self.to_board.clear()
        commands = (b"capture_stop", STOP_PLAYBACK)
        if self.peer_volume is not None:
            commands += (self.peer_volume,)
        self.to_board.put(tuple(make_packet(CONTROL, command) for command in commands))

    def set_peer(self, address):
        if self.peer is not None and self.peer != address and self.session is not None:
            self.inform_peer(LINK_LOST)
        self.peer = address
        self.peer_seen = 0.0
        self.peer_epoch += 1
        self.peer_volume = None
        self.to_backend.clear()
        self.assembler.reset()
        if self.session is not None:
            self.reset_board_audio()
        if address is not None:
            LOG.info("Backend peer is 127.0.0.1:%d", address[1])

    def datagram_received(self, data, address):
        if address[0] != "127.0.0.1" or not valid_packet(data):
            self.invalid_udp += 1
            return
        control = control_payload(data)
        if control == BACKEND_GOODBYE:
            if address == self.peer:
                self.set_peer(None)
            return

        now = time.monotonic()
        if control == BACKEND_HELLO or address != self.peer:
            if (control != BACKEND_HELLO and self.peer is not None
                    and now - self.peer_seen < PEER_TIMEOUT):
                return
            self.set_peer(address)
        self.peer_seen = now
        if control is not None and control.startswith(b"volume:"):
            volume = control.removeprefix(b"volume:")
            if not 1 <= len(volume) <= 3 or not volume.isdigit() or int(volume) > 100:
                return
            self.peer_volume = control
        if control in (BACKEND_HELLO, b"ping"):
            self.inform_peer(LINK_READY if self.session is not None else LINK_LOST)
            if self.peer_volume is None:
                self.inform_peer(VOLUME_REQUEST)
        if self.session is None or control == BACKEND_HELLO:
            return
        if control == STOP_PLAYBACK:
            self.to_board.discard_playback()
        queued = self.to_board.put(bytes(data), shed_audio=control != b"ping")
        if not queued and not is_audio(data) and control != b"ping":
            self.session_ended.set()

    def notification(self, session, data):
        if session is not self.session:
            return
        packet = self.assembler.feed(data)
        if packet is None or self.peer is None or control_payload(packet) in (
            BACKEND_HELLO, BACKEND_GOODBYE, LINK_READY, VOLUME_REQUEST
        ):
            return
        if not self.to_backend.put(packet) and not is_audio(packet):
            self.session_ended.set()

    def error_received(self, error):
        LOG.warning("Local UDP error %r", error)

    async def forward_udp(self):
        while True:
            packet = await self.to_backend.get()
            if self.peer is not None:
                self.transport.sendto(packet, self.peer)
                self.sent_udp += 1

    async def monitor(self):
        reported_at = time.monotonic()
        while True:
            await asyncio.sleep(1)
            if self.peer is not None and time.monotonic() - self.peer_seen >= PEER_TIMEOUT:
                self.set_peer(None)
                LOG.info("Backend peer expired, capture and playback stopped")
            if time.monotonic() - reported_at >= 10:
                reported_at = time.monotonic()
                LOG.info(
                    "Frames PC->BLE=%d BLE->PC=%d dropped=%d invalid=%d",
                    self.sent_ble, self.sent_udp,
                    self.to_board.dropped + self.to_backend.dropped,
                    self.invalid_udp + self.assembler.invalid,
                )


async def write_packet(client, characteristic, packet):
    value_size = min(characteristic.max_write_without_response_size, MAX_GATT_VALUE_BYTES)
    for fragment in fragments(packet, value_size):
        # One writer serializes complete packets. Only control endings use an ATT response.
        response = packet[2] not in AUDIO_TYPES and bool(fragment[0] & LAST)
        await asyncio.wait_for(client.write_gatt_char(characteristic, fragment, response=response), timeout=5)


async def forward_ble(client, characteristic, bridge):
    while True:
        entry = await bridge.to_board.get()
        peer_epoch = bridge.peer_epoch
        for packet in (entry,) if isinstance(entry, bytes) else entry:
            if peer_epoch != bridge.peer_epoch:
                break
            await write_packet(client, characteristic, packet)
            bridge.sent_ble += 1


class DeviceSelectionError(RuntimeError):
    pass


async def find_device(scanner, identifier, timeout):
    discovered = await scanner.discover(timeout=timeout, return_adv=True, service_uuids=[SERVICE_UUID])
    matches = []
    for device, advertisement in discovered.values():
        names = (device.address, device.name or "", advertisement.local_name or "")
        if identifier is None or any(identifier.casefold() == name.casefold() for name in names):
            matches.append(device)
    if not matches:
        raise ConnectionError("Voice board was not found in BLE advertisements")
    if len(matches) != 1:
        choices = ", ".join(f"{device.address} {device.name!r}" for device in matches)
        raise DeviceSelectionError(f"Multiple voice boards found. Use --device with an address. {choices}")
    return matches[0]


async def connected_session(client, bridge):
    rx = client.services.get_characteristic(RX_UUID)
    tx = client.services.get_characteristic(TX_UUID)
    if rx is None or tx is None or not {"write", "write-without-response"}.issubset(rx.properties):
        raise RuntimeError("Voice BLE service is missing its required RX/TX characteristics")
    if "notify" not in tx.properties:
        raise RuntimeError("Voice TX characteristic does not support notifications")
    session = object()
    bridge.reset()
    await client.start_notify(tx, lambda _, data: bridge.notification(session, data))
    bridge.session = session
    tasks = []
    try:
        if bridge.peer is not None and time.monotonic() - bridge.peer_seen < PEER_TIMEOUT:
            bridge.reset_board_audio()
            bridge.inform_peer(LINK_READY)
        LOG.info("Paired BLE voice link ready, GATT value limit %d bytes", min(rx.max_write_without_response_size, MAX_GATT_VALUE_BYTES))
        tasks = [
            asyncio.create_task(forward_ble(client, rx, bridge)),
            asyncio.create_task(bridge.forward_udp()),
            asyncio.create_task(bridge.monitor()),
            asyncio.create_task(bridge.session_ended.wait()),
        ]
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        bridge.end_session(session)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if client.is_connected and bridge.peer is not None:
            async def stop_board():
                for command in (b"capture_stop", b"stop_playback"):
                    await write_packet(client, rx, make_packet(CONTROL, command))
            with suppress(Exception):
                await asyncio.wait_for(stop_board(), timeout=2)


async def run(args):
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise RuntimeError("Install BLE requirements with python -m pip install -r voice/tools/requirements-ble.txt") from error

    loop = asyncio.get_running_loop()
    bridge = UdpBridge(args.queue_packets)
    transport, _ = await loop.create_datagram_endpoint(
        lambda: bridge, local_addr=("127.0.0.1", args.udp_port)
    )
    LOG.info("Listening at 127.0.0.1:%d, discovering paired BLE voice service", args.udp_port)
    LOG.info("Start voice --ble --esp32-port %d. Ctrl+C exits.", args.udp_port)
    identifier = args.device
    retry_delay = 1
    try:
        while True:
            bridge.reset()
            ended = asyncio.Event()
            bridge.session_ended = ended
            try:
                device = await find_device(BleakScanner, identifier, args.scan_timeout)
                identifier = device.address
                LOG.info("Connecting and pairing with %r (%r)", device.name, device.address)
                async with BleakClient(
                    device, pair=True, timeout=60, services=[SERVICE_UUID],
                    disconnected_callback=lambda _, event=ended: event.set(),
                ) as client:
                    retry_delay = 1
                    await connected_session(client, bridge)
                LOG.info("BLE link disconnected, scanning again")
            except DeviceSelectionError:
                raise
            except Exception as error:
                LOG.warning("BLE connection failed %r. Retrying in %d seconds", error, retry_delay)
            finally:
                bridge.reset()
            await asyncio.sleep(retry_delay)
            retry_delay = min(retry_delay * 2, 10)
    finally:
        bridge.reset()
        transport.close()


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", help="Exact BLE device address or advertised name")
    parser.add_argument("--udp-port", type=int, default=5000, help="Local voice UDP port (default 5000)")
    parser.add_argument("--queue-packets", type=int, default=32, help="Each direction's bounded packet queue (default 32)")
    parser.add_argument("--scan-timeout", type=float, default=5, help="BLE scan time in seconds (default 5)")
    args = parser.parse_args()
    if not 1 <= args.udp_port <= 65535 or not 2 <= args.queue_packets <= 256:
        parser.error("UDP port must be 1..65535 and queue size must be 2..256")
    if not math.isfinite(args.scan_timeout) or not 1 <= args.scan_timeout <= 60:
        parser.error("Scan timeout must be 1..60 seconds")
    return args


def main():
    args = arguments()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        LOG.info("BLE voice bridge stopped")
        return 0
    except Exception as error:
        LOG.error("Bridge stopped %r", error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
