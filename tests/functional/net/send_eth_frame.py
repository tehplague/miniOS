#!/usr/bin/env python3
"""Send a raw Ethernet frame to a QEMU socket NIC backend via UDP.

Usage:
  send_eth_frame.py --ethertype 0x0806 --port 19000
  send_eth_frame.py --ethertype 0x0800 --port 19000 --host 127.0.0.1

Each invocation sends one complete Ethernet frame (64 bytes, broadcast dst)
to the QEMU socket NIC UDP port so the emulated guest NIC receives it.
"""

import argparse
import socket
import struct
import sys


def build_frame(ethertype: int) -> bytes:
    """Build a minimal valid Ethernet frame (64 bytes including FCS placeholder)."""
    dst_mac = b'\xff\xff\xff\xff\xff\xff'          # broadcast
    src_mac = b'\x52\x54\x00\x11\x22\x33'          # injector MAC (test-only)
    eth_type = struct.pack('>H', ethertype)

    # Minimal payload to reach 60-byte minimum frame body (excl. FCS)
    payload = b'\x00' * 46

    frame = dst_mac + src_mac + eth_type + payload  # 6+6+2+46 = 60 bytes
    return frame


def main() -> int:
    parser = argparse.ArgumentParser(description='Inject Ethernet frame into QEMU socket NIC')
    parser.add_argument('--ethertype', required=True,
                        type=lambda x: int(x, 16),
                        help='Ethertype in hex, e.g. 0x0806 for ARP')
    parser.add_argument('--host', default='127.0.0.1',
                        help='QEMU socket NIC host (default: 127.0.0.1)')
    parser.add_argument('--port', type=int, required=True,
                        help='QEMU socket NIC UDP port (localaddr port)')
    args = parser.parse_args()

    frame = build_frame(args.ethertype)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(frame, (args.host, args.port))
    finally:
        sock.close()

    print(f'Sent {len(frame)}-byte frame ethertype=0x{args.ethertype:04x} '
          f'to {args.host}:{args.port}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
