#!/usr/bin/env python3
"""Desk-side BLE client for the PCGKolect GATT service (PROTOCOL.md §3).

usage:
  ble_client.py [--name PCGKolect] [--tlm SECONDS] [--pair] 'op[ json-args]' ...
examples:
  ble_client.py hello status
  ble_client.py 'set_dataset {"dataset_id":"...","name":"DS","rev":1,"settings":{"target_usable_s":20}}'
  ble_client.py --tlm 10 status          # also print telemetry for 10 s

Needs: pip install bleak (Linux/BlueZ, macOS or Windows).
"""
import argparse
import asyncio
import json
import sys
import time

from bleak import BleakClient, BleakScanner

BASE = '7a1c00{:02x}-5b6e-4e8a-9c1f-50434b4f4c54'
SVC, INFO, CMD, RSP, TLM, WIFI = (BASE.format(i) for i in range(1, 7))


class Reassembler:
    """Joins fragments (HDR bit7 FIRST, bit6 LAST) into complete JSON messages."""

    def __init__(self, on_message):
        self.buf = bytearray()
        self.on_message = on_message

    def __call__(self, _char, data: bytearray):
        hdr = data[0]
        if hdr & 0x80:
            self.buf = bytearray()
        self.buf += data[1:]
        if hdr & 0x40:
            try:
                self.on_message(json.loads(self.buf.decode()))
            except ValueError:
                print('!! bad JSON', bytes(self.buf)[:120])


def fragments(payload: bytes, chunk: int):
    out = []
    for i, off in enumerate(range(0, max(len(payload), 1), chunk)):
        part = payload[off:off + chunk]
        hdr = (0x80 if off == 0 else 0) | (0x40 if off + chunk >= len(payload) else 0) | (i & 0x3F)
        out.append(bytes([hdr]) + part)
    return out


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', default='PCGKolect')
    ap.add_argument('--tlm', type=float, default=0, help='print telemetry for N seconds after the commands')
    ap.add_argument('--pair', action='store_true', help='pair/bond before sending commands')
    ap.add_argument('--timeout', type=float, default=6.0)
    ap.add_argument('cmds', nargs='*')
    a = ap.parse_args()

    dev = await BleakScanner.find_device_by_filter(
        lambda d, adv: (d.name or '').startswith(a.name), timeout=10)
    if dev is None:
        sys.exit(f'no device named {a.name}*')
    print('device', dev.name, dev.address)

    pending = {}
    loop = asyncio.get_running_loop()

    def on_rsp(msg):
        fut = pending.pop(msg.get('id'), None)
        if fut and not fut.done():
            fut.set_result(msg)
        else:
            print('<< (unmatched)', msg)

    def on_tlm(msg):
        print(f'tlm {time.strftime("%H:%M:%S")}', json.dumps(msg)[:300])

    async with BleakClient(dev) as cl:
        if a.pair:
            await cl.pair()
        print('mtu', cl.mtu_size)
        await cl.start_notify(RSP, Reassembler(on_rsp))
        await cl.start_notify(TLM, Reassembler(on_tlm) if a.tlm else (lambda *_: None))
        await cl.start_notify(WIFI, Reassembler(lambda m: print('wifi', m)))
        info = await cl.read_gatt_char(INFO)
        print('info', info.decode()[:400])
        chunk = max(20, cl.mtu_size - 3) - 1
        rid = int(time.time()) & 0xFFFFFF
        for c in a.cmds:
            op, _, args = c.partition(' ')
            req = {'v': 1, 'id': rid, 'op': op, 'utc_ms': int(time.time() * 1000)}
            if args:
                req.update(json.loads(args))
            fut = loop.create_future()
            pending[rid] = fut
            t0 = time.time()
            for f in fragments(json.dumps(req, separators=(',', ':')).encode(), chunk):
                await cl.write_gatt_char(CMD, f, response=True)
            try:
                msg = await asyncio.wait_for(fut, a.timeout)
                print(f'<< {op} ({(time.time() - t0) * 1000:.0f} ms)', json.dumps(msg)[:600])
            except asyncio.TimeoutError:
                print(f'<< {op}: no response')
            rid += 1
        if a.tlm:
            await asyncio.sleep(a.tlm)


if __name__ == '__main__':
    asyncio.run(main())
