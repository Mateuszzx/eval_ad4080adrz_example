# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
"""Capture AD4080 samples from the SDP-K1 SDRAM buffer.

The board fills external RAM with one FIFO at a time before it sends
anything. The converter has a single 16384-sample FIFO, so each burst
is gap-free at the CNV rate and the next burst waits for the SPI DMA
readout, about 47 ms at 10.5 MHz. The buffer then comes out of the
USB CDC device (VID 0x1209, PID 0x4080). That link enumerates at full
speed. It is not the DAPLink serial port.

    from ad4080 import Ad4080

    with Ad4080() as adc:
        adc.save("capture.npy")
"""

from __future__ import annotations

import argparse
import array
import struct
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

_REQ = b"\xa5\x5a"
_RESP = b"\x5a\xa5"
_SAMPLE_BYTES = 3
_INFO_VALUES = 3
_HDR_BYTES = 6
USB_VID = 0x1209
USB_PID = 0x4080
# One byte in the sign table, built once. bytes.translate does the rest.
_SIGN = bytes(0xFF if i & 0x80 else 0x00 for i in range(256))


class Ad4080Error(RuntimeError):
    pass


def find_port() -> str:
    matches = [
        port.device
        for port in list_ports.comports()
        if port.vid == USB_VID and port.pid == USB_PID
    ]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise Ad4080Error("AD4080 USB device not found (1209:4080)")
    raise Ad4080Error(f"several AD4080 USB devices: {', '.join(matches)}")


class Ad4080:
    def __init__(self, port: str | None = None, timeout: float = 30.0):
        self.port = port or find_port()
        self.timeout = timeout
        # The CDC baud rate is ignored. The bytes move at the USB full-speed rate.
        self._ser = serial.Serial(self.port, 115200, timeout=0.2, exclusive=True)
        self._ser.reset_input_buffer()
        self.fifo_samples, self.max_samples, self.sample_hz = self._read_info()

    def close(self) -> None:
        if self._ser is not None:
            self._ser.close()
            self._ser = None

    def __enter__(self) -> "Ad4080":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def capture(self, count: int | None = None) -> array.array:
        """Read one SDRAM fill of signed 20-bit codes, stored as int32.

        The record is FIFO bursts placed back to back. It is not one
        gap-free capture once count is larger than fifo_samples.
        """
        if count is None:
            count = self.max_samples
        if count < 1 or count > self.max_samples:
            raise ValueError(f"count must be 1..{self.max_samples}")

        if count > self.fifo_samples:
            print(f"filling SDRAM, {count} samples", flush=True)
        payload_count, status, payload = self._request(count)
        if status != 0:
            raise Ad4080Error(f"capture failed: {status}")
        if payload_count != count or len(payload) != count * _SAMPLE_BYTES:
            raise Ad4080Error(f"short record: got {payload_count}, expected {count}")
        return _load_i24(payload)

    def save(
        self,
        path: str | Path,
        count: int | None = None,
        repeats: int = 1,
        total: int | None = None,
    ) -> int:
        """Write codes to a .npy array or a raw little-endian int32 file.

        Each USB transfer is one SDRAM fill, at most max_samples. total
        samples are split into those fills. Inside a fill the board
        stops sampling before it sends.
        """
        chunk = count or self.max_samples
        if chunk < 1 or chunk > self.max_samples:
            raise ValueError(f"count must be 1..{self.max_samples}")
        if total is None:
            if repeats < 1:
                raise ValueError("repeats must be >= 1")
            total = chunk * repeats
        if total < 1:
            raise ValueError("total must be >= 1")

        dest = Path(path)
        written = 0
        started = time.monotonic()
        with dest.open("wb") as out:
            if dest.suffix == ".npy":
                out.write(_npy_header(total))
            remaining = total
            while remaining:
                block = self.capture(min(chunk, remaining))
                data = block.tobytes()
                if sys.byteorder != "little":
                    ordered = array.array("i", block)
                    ordered.byteswap()
                    data = ordered.tobytes()
                out.write(data)
                written += len(block)
                remaining -= len(block)
                elapsed = time.monotonic() - started
                rate = written / elapsed if elapsed else 0
                print(f"{written} / {total} samples, {rate:,.0f}/s", flush=True)
        return written

    def _write(self, data: bytes) -> None:
        self._ser.write(data)
        self._ser.flush()

    def _read(self, size: int, deadline: float) -> bytes:
        chunks = bytearray()
        started = time.monotonic()
        last_byte = started
        last_report = started
        while len(chunks) < size:
            now = time.monotonic()
            if now > deadline:
                raise Ad4080Error(
                    f"timed out waiting for the capture ({len(chunks)} of {size} bytes)"
                )
            if size >= 1_000_000 and chunks and now - last_report >= 0.5:
                rate = len(chunks) / (now - started) if now > started else 0.0
                print(f"  {len(chunks)} / {size} bytes, {rate / 1e6:.1f} MB/s", flush=True)
                last_report = now
            self._ser.timeout = min(0.2, max(0.01, deadline - now))
            part = self._ser.read(size - len(chunks))
            if not part:
                if chunks and time.monotonic() - last_byte > 2.0:
                    raise Ad4080Error(f"USB link stalled ({len(chunks)} of {size} bytes)")
                continue
            chunks += part
            last_byte = time.monotonic()
        return bytes(chunks)

    def _read_info(self) -> tuple[int, int, int]:
        count, status, payload = self._request(0)
        if status != 0 or count != _INFO_VALUES or len(payload) != _INFO_VALUES * 4:
            raise Ad4080Error(f"board did not answer the info request ({status})")
        fifo, ram, rate = struct.unpack("<iii", payload)
        if fifo < 1 or ram < fifo or rate < 1:
            raise Ad4080Error(f"board reported limits {fifo}, {ram}, {rate}")
        return fifo, ram, rate

    def _request(self, count: int) -> tuple[int, int, bytes]:
        # Info is answered at once. A sample request stays silent while the
        # SDRAM fills, so the wait covers that before the header exists.
        # Info values are 4 bytes each. Samples are 3 bytes each.
        width = 4 if count == 0 else _SAMPLE_BYTES
        deadline = time.monotonic() + self._header_wait(count)
        self._write(_REQ + struct.pack("<I", count))
        return self._read_frame(deadline, width)

    def _header_wait(self, count: int) -> float:
        if count == 0:
            return 2.0
        fifos = (count + self.fifo_samples - 1) // self.fifo_samples
        # Each burst is one FIFO: the DMA readout is about 50 ms, then the
        # samples are stored. The header is not sent until the last burst.
        return 5.0 + fifos * 0.25

    def _read_frame(self, sync_deadline: float, width: int) -> tuple[int, int, bytes]:
        window = b""
        started = time.monotonic()
        last_note = started
        long_fill = sync_deadline - started > 5.0
        while True:
            now = time.monotonic()
            if now > sync_deadline:
                raise Ad4080Error("timed out waiting for a response")
            if long_fill and now - last_note >= 2.0:
                print("  still filling SDRAM", flush=True)
                last_note = now
            self._ser.timeout = min(0.2, max(0.01, sync_deadline - now))
            part = self._ser.read(65536)
            if not part:
                continue
            window += part
            start = window.find(_RESP)
            if start < 0:
                window = window[-1:]
                continue
            rest = window[start + len(_RESP) :]
            break

        header = rest
        if len(header) < _HDR_BYTES:
            header += self._read(_HDR_BYTES - len(header), sync_deadline + 2.0)
        count, status = struct.unpack_from("<Ih", header)
        if status != 0:
            return count, int(status), b""
        need = count * width
        payload = header[_HDR_BYTES :]
        # A full SDRAM buffer is about 16 MB. 1 MB/s still finishes it,
        # and a stalled link hits the deadline instead of waiting forever.
        payload_deadline = time.monotonic() + max(10.0, need / 500_000 + 10.0)
        if len(payload) < need:
            payload += self._read(need - len(payload), payload_deadline)
        return count, int(status), payload[:need]


def _load_i24(payload: bytes) -> array.array:
    count = len(payload) // _SAMPLE_BYTES
    raw = bytearray(count * 4)
    raw[0::4] = payload[0::3]
    raw[1::4] = payload[1::3]
    raw[2::4] = payload[2::3]
    raw[3::4] = payload[2::3].translate(_SIGN)
    samples = array.array("i")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def _npy_header(count: int) -> bytes:
    header = f"{{'descr': '<i4', 'fortran_order': False, 'shape': ({count},), }}".encode()
    pad = (64 - ((10 + len(header) + 1) % 64)) % 64
    header += b" " * pad + b"\n"
    return b"\x93NUMPY" + bytes((1, 0)) + struct.pack("<H", len(header)) + header


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Save an AD4080 capture from SDRAM over USB.")
    parser.add_argument("path", help="output .npy or raw int32 file")
    parser.add_argument("-p", "--port", help="CDC port (default: the 1209:4080 device)")
    parser.add_argument(
        "-n",
        "--count",
        type=int,
        help="samples per USB transfer (default: one full SDRAM buffer)",
    )
    parser.add_argument("-r", "--repeats", type=int, default=1, help="transfers when --samples is omitted")
    parser.add_argument("-N", "--samples", type=int, help="total samples, split into SDRAM fills")
    args = parser.parse_args(argv)

    with Ad4080(args.port) as adc:
        written = adc.save(args.path, args.count, args.repeats, args.samples)
        bursts = (written + adc.fifo_samples - 1) // adc.fifo_samples
        print(
            f"wrote {written} samples "
            f"({bursts} FIFO bursts of up to {adc.fifo_samples} at {adc.sample_hz / 1e6:.0f} MHz) "
            f"to {args.path}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
