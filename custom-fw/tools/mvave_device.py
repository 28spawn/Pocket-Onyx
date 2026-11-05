#!/usr/bin/env python3
"""
custom-fw/tools/mvave_device.py - talk to a Pocket Amp over USB MIDI.

The USB MIDI SysEx protocol, as the firmware implements it
(custom-fw/src/sys/sysex.c) and standard device communication framing.

The frame:

    F0 00 59 <cmd> <len:u24le> <payload> <chk> F7

    chk = ~sum(cmd, len bytes, payload) & 0x7F

An upload is a SESSION:

    1. cmd 25 (WRITE_BEGIN) arms it. The payload is the declared length as a
       u24le, then the target type (1 = a NAM capture) and the slot.
    2. Every frame after that is a body chunk. The firmware takes the payload
       of ANY other command as body bytes, up to 256 a frame.
    3. When the declared length has arrived the firmware commits the upload:
       the capture is registered, stored in the flash and put in the DSP.

The target of this module is the CUSTOM firmware, because that is the one
whose protocol is known exactly - it is in the source. Two things to know:

  * The framing is the same on both devices: the inner frame is 8-bit binary
    and travels packed 8-to-7-bit between F0 and F7, because USB MIDI carries
    7-bit data bytes. The stock device unpacks that in its transport, and so
    does the custom firmware now (it used to mask every byte with 0x7F, which
    made a binary capture arrive with its top bits stripped - a capture could
    not be sent at all).

  * The upload sequence uses the cmd 25 upload session, which is verified
    and safe.

Only the READ commands (17, 18, 21, 35) and the upload session are used
here. A wrong write can corrupt the device's preset store, so --probe (a
device-info read) is the safe way to check that the link works.

    python custom-fw/tools/mvave_device.py --list
    python custom-fw/tools/mvave_device.py --probe
    python custom-fw/tools/mvave_device.py --upload capture.nam
"""
import argparse
import base64
import os
import struct
import sys

# --- the protocol -----------------------------------------------------------
SYSEX_START = 0xF0
SYSEX_END = 0xF7
HDR0 = 0x00
HDR1 = 0x59
MAX_PAYLOAD = 256

CMD_DEVICE_INFO = 17
CMD_DESCRIPTOR = 18
CMD_DRAIN = 21
CMD_WRITE_BEGIN = 25
CMD_WRITE_CHUNK = 34      # the editor's write command; the firmware's
                          # dispatcher ignores the command of a body frame
CMD_READ = 35
CMD_COMMIT = 48

UPLOAD_PRESET = 0
UPLOAD_NAM = 1
UPLOAD_CAB = 2
UPLOAD_SETTINGS = 3

# The preset record, pa_preset_t (include/pa_preset.h): 164 bytes.
PRESET_RECORD_SIZE = 164
PRESET_MAGIC = 0x32504D50            # "PMP2"
PRESET_NAME_LEN = 24
PRESET_SLOT_COUNT = 8
PRESET_NPARAMS = 6
PRESET_COUNT = 80


class ProtocolError(Exception):
    pass


def build_frame(cmd, payload=b""):
    """The SysEx frame on the wire, exactly as pa_sysex_build_frame() builds it.

    The inner frame is 8-bit binary:

        00 59 <cmd> <len:u24le> <payload> <chk>
        chk = ~sum(cmd, len bytes, payload) & 0xFF

    and it travels packed 8-to-7-bit between F0 and F7, because USB MIDI
    carries 7-bit data bytes. The device unpacks that in its transport
    and so does the custom firmware now, so the same frame works on both.
    """
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError("a frame carries at most %d payload bytes" % MAX_PAYLOAD)
    # the length is a 24-bit little-endian word (len:u24le)
    l0 = len(payload) & 0xFF
    l1 = (len(payload) >> 8) & 0xFF
    l2 = (len(payload) >> 16) & 0xFF
    total = (cmd & 0xFF) + l0 + l1 + l2
    body = bytes(payload)
    total += sum(body)
    inner = bytearray([HDR0, HDR1, cmd & 0xFF, l0, l1, l2])
    inner += body
    inner.append((~total) & 0xFF)
    return bytes([SYSEX_START]) + pack_8to7(bytes(inner)) + bytes([SYSEX_END])


def pack_8to7(data):
    """The standard LSB-first 8-to-7-bit MIDI packing used by the transport.

    The bytes go into one bit stream, LSB first, and come out again in
    7-bit groups. The last group is padded with zeros.
    """
    bits = 0
    nbits = 0
    out = bytearray()
    for b in data:
        bits |= (b & 0xFF) << nbits
        nbits += 8
        while nbits >= 7:
            out.append(bits & 0x7F)
            bits >>= 7
            nbits -= 7
    if nbits:
        out.append(bits & 0x7F)
    return bytes(out)


def unpack_7to8(data):
    """The inverse of pack_8to7 (what the device does)."""
    bits = 0
    nbits = 0
    out = bytearray()
    for b in data:
        bits |= (b & 0x7F) << nbits
        nbits += 7
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8
    return bytes(out)


# --- the BLE-MIDI 1.0 framing ------------------------------------------------
# A BLE-MIDI peer does not send bare MIDI bytes. Every packet is
#
#     byte 0    the header: 0x80 | (timestamp >> 7); the low 6 bits carry the
#               timestamp's high bits
#     byte 1    the timestamp byte: 0x80 | (timestamp & 0x7F), the high bit
#               set. The byte AFTER it is unconditionally the first byte of a
#               MIDI message - it may be a status byte
#     byte 2..  the message
#
# and a SysEx that spans packets is sent as continuation packets, which carry
# raw data and NO timestamp byte. The closing F7 is always preceded by a
# timestamp byte (standard BLE-MIDI 1.0).

def ble_midi_packets(message, mtu=23):
    """Frame a MIDI message as BLE-MIDI 1.0 packets, the way a peer sends it.

    This mirrors the device's own framer (custom-fw/src/board/ble_midi.c), so
    a message framed here is what the device's parser expects to receive.
    `mtu` is the negotiated ATT MTU (the BLE minimum is 23), so a write
    payload is mtu - 3 bytes - 20 with the minimum, which is what the
    upstream's own sender uses.
    """
    if mtu < 23:
        mtu = 23
    chunk_max = mtu - 3                 # the ATT write payload
    if chunk_max < 4:
        chunk_max = 4

    if message[0] != 0xF0 or message[-1] != 0xF7:
        # A plain MIDI message goes out in ONE packet: the timestamp byte
        # introduces it, and the continuation packets - which carry raw data
        # and no timestamp - are a SysEx thing.
        take = min(len(message), chunk_max - 2)
        return [bytes([0x80, 0x80]) + message[:take]]

    # The closing F7 is always preceded by a timestamp byte, so it is held
    # back and emitted with one.
    hold_f7 = True
    total = len(message) - 1
    out = []
    pos = 0
    first = True
    while pos < total:
        pkt = bytearray([0x80])          # the header: timestamp 0
        if first:
            pkt.append(0x80)              # the timestamp byte
            first = False
        room = chunk_max - len(pkt)
        take = min(total - pos, room)
        # the closing F7 needs two more bytes in this packet
        if pos + take >= total and take + 2 > room:
            take = room - 2 if room >= 3 else 0
        pkt += message[pos:pos + take]
        pos += take
        if hold_f7 and pos >= total:
            pkt.append(0x80)              # the timestamp before the F7
            pkt.append(0xF7)
        out.append(bytes(pkt))
        if take == 0 and pos < total:
            break
    if hold_f7 and (not out or out[-1][-1] != 0xF7):
        # the F7 did not fit in the last data packet: it goes out on its own
        out.append(bytes([0x80, 0x80, 0xF7]))
    return out


def ble_midi_unframe(packets):
    """The inverse: BLE-MIDI packets in, the MIDI message out.

    It mirrors the device's parser (custom-fw/src/board/ble_midi.c) exactly,
    so a message that survives here survives on the device.
    """
    state = 0                          # 0 = a message, 1 = a timestamp in a
    in_sysex = 0                       #    SysEx, 2 = a timestamp in a message
    out = bytearray()

    for pkt in packets:
        if len(pkt) < 2:
            continue
        # A peer that was interrupted and started a NEW SysEx sends
        # [header][timestamp][F0 ...] - and a SysEx's payload can never
        # contain an F0, so that is unambiguous: the old frame was lost.
        fresh_sysex = (len(pkt) >= 3 and (pkt[1] & 0x80) and pkt[2] == 0xF0)
        if state == 1:
            # the byte after a timestamp inside a SysEx: the closing F7 or a
            # System Real-Time byte (which is dropped)
            state = 0
            if fresh_sysex:
                in_sysex = 0
                i = 2
            else:
                if pkt[1] == 0xF7:
                    out.append(0xF7)
                    in_sysex = 0
                elif pkt[1] < 0xF8:
                    # neither: the timestamp was spurious, so take the byte
                    # as data rather than lose it
                    out.append(pkt[1])
                i = 2
        elif state == 2:
            # the byte after a timestamp: the message's first byte
            state = 0
            out.append(pkt[1])
            i = 2
        elif in_sysex:
            if fresh_sysex:
                # the previous frame was lost: this packet starts a new one
                in_sysex = 0
                i = 2
            else:
                i = 1                  # a continuation packet: no timestamp
        else:
            i = 2                      # the header and the timestamp
            if len(pkt) == 2:
                # the packet carried the header and the timestamp and nothing
                # else: the message's first byte is in the next packet
                state = 2
                continue
        while i < len(pkt):
            b = pkt[i]
            i += 1
            if not in_sysex:
                out.append(b)
                if b == 0xF0:
                    in_sysex = 1
                continue
            if b == 0xF7:
                # the closing F7: unambiguous inside a SysEx (the data is
                # 7-bit), even without the timestamp before it
                out.append(0xF7)
                in_sysex = 0
                continue
            if b & 0x80:
                # a timestamp byte: framing, not payload. The byte after it is
                # the closing F7 or a System Real-Time byte.
                if i >= len(pkt):
                    state = 1
                    break
                b = pkt[i]
                i += 1
                if b == 0xF7:
                    out.append(0xF7)
                    in_sysex = 0
                continue
            out.append(b)
    return bytes(out)


def build_preset_record(name="", preset_vol=80, slot_order=None, amp_model_idx=0,
                        cab_model_idx=0, params=None, slot_type=None,
                        slot_enable_mask=0xFF, bpm=120, genre_tag=0,
                        input_trim_db=0, gate_threshold_pct=50,
                        custom_cab_idx=255, custom_cab_blend=0,
                        stomp_map=None):
    """The preset record, pa_preset_t (164 bytes), exactly as the firmware
    stores it (include/pa_preset.h).

    The firmware rewrites the magic, the sequence number, the EMPTY flag and
    the CRC32 when it saves the slot, so only the fields that matter have to
    be filled in here. The knob values are Q16 (0..65535 = 0..1.0).

    This is what a cmd-25 session with target 0 (PA_UPLOAD_PRESET) carries.
    """
    import struct

    name_bytes = name.encode("ascii", "replace")[:PRESET_NAME_LEN - 1]
    order = list(slot_order) if slot_order else list(range(PRESET_SLOT_COUNT))
    types = list(slot_type) if slot_type else [0] * PRESET_SLOT_COUNT
    stomp = list(stomp_map) if stomp_map else [0, 0, 0, 0]
    knobs = []
    for slot in range(PRESET_SLOT_COUNT):
        row = list(params[slot]) if params else [0] * PRESET_NPARAMS
        knobs += [int(max(0, min(65535, v))) for v in row[:PRESET_NPARAMS]]
        knobs += [0] * (PRESET_NPARAMS - len(row[:PRESET_NPARAMS]))

    rec = bytearray(PRESET_RECORD_SIZE)
    struct.pack_into("<I", rec, 0, PRESET_MAGIC)
    struct.pack_into("<I", rec, 4, 0)                       # the sequence
    rec[8:8 + len(name_bytes)] = name_bytes
    struct.pack_into("<H", rec, 32, bpm & 0xFFFF)
    rec[34] = genre_tag & 0xFF
    rec[35] = custom_cab_idx & 0xFF
    rec[36] = custom_cab_blend & 0xFF
    rec[37] = input_trim_db & 0xFF if input_trim_db >= 0 else (256 + input_trim_db)
    rec[38] = gate_threshold_pct & 0xFF
    rec[39] = slot_enable_mask & 0xFF
    rec[40:40 + PRESET_SLOT_COUNT] = bytes(o & 0xFF for o in order[:PRESET_SLOT_COUNT])
    rec[48] = amp_model_idx & 0xFF
    rec[49] = cab_model_idx & 0xFF
    rec[50] = max(0, min(100, preset_vol))
    rec[51] = 0                                             # not EMPTY
    rec[52:52 + PRESET_SLOT_COUNT] = bytes(t & 0xFF for t in types[:PRESET_SLOT_COUNT])
    rec[60:64] = bytes(s & 0xFF for s in stomp[:4])
    struct.pack_into("<%dH" % (PRESET_SLOT_COUNT * PRESET_NPARAMS), rec, 64, *knobs)
    # the CRC32 is over the record minus the CRC field itself; the firmware
    # recomputes it, so the value here only has to be well-formed.
    import zlib
    struct.pack_into("<I", rec, 160, zlib.crc32(bytes(rec[:160])) & 0xFFFFFFFF)
    return bytes(rec)


def unpack_preset_record(blob):
    """The fields of a preset record, for reading one back."""
    import struct
    if len(blob) < PRESET_RECORD_SIZE:
        raise ProtocolError("a preset record is %d bytes, this is %d"
                            % (PRESET_RECORD_SIZE, len(blob)))
    return {
        "magic": struct.unpack_from("<I", blob, 0)[0],
        "seq": struct.unpack_from("<I", blob, 4)[0],
        "name": blob[8:8 + PRESET_NAME_LEN].split(b"\x00")[0].decode("ascii", "replace"),
        "bpm": struct.unpack_from("<H", blob, 32)[0],
        "preset_vol": blob[50],
        "slot_enable_mask": blob[39],
        "slot_order": list(blob[40:48]),
        "amp_model_idx": blob[48],
        "cab_model_idx": blob[49],
        "flags": blob[51],
    }


# --- the link ---------------------------------------------------------------
class DeviceLink:
    """A USB-MIDI link to the device."""

    def __init__(self, port=None, timeout=2.0):
        self.port_name = port
        self.timeout = timeout
        self._out = None
        self._in = None

    # ---- the ports ----
    @staticmethod
    def ports():
        """(outputs, inputs) - the MIDI ports on this machine."""
        import rtmidi
        out = rtmidi.MidiOut()
        inn = rtmidi.MidiIn()
        return out.get_ports(), inn.get_ports()

    def _open(self):
        if self._out is not None:
            return
        import rtmidi
        self._out = rtmidi.MidiOut()
        outs = self._out.get_ports()
        if not outs:
            raise ProtocolError("there is no MIDI output on this machine")
        name = self.port_name
        if name is None:
            name = outs[0]
        if name not in outs:
            raise ProtocolError("no MIDI output called %r. The ones here: %s"
                               % (name, ", ".join(outs)))
        # rtmidi's open_port() takes the INDEX of the port, not its name.
        self._out.open_port(outs.index(name))
        self._port_used = name

    def _open_input(self):
        import rtmidi
        self._in = rtmidi.MidiIn()
        ins = self._in.get_ports()
        if not ins:
            return None
        name = self.port_name
        # the input that matches the output's device, if there is one
        if name:
            guess = None
            for p in ins:
                base = name.replace(" Out", "").replace("MIDI Out", "MIDI In")
                if base.lower() in p.lower():
                    guess = p
                    break
            name = guess or ins[0]
        else:
            name = ins[0]
        self._in.open_port(ins.index(name))
        self._in.ignore_types(sysex=False, timing=True, active_sense=True)
        return name

    def close(self):
        for h in (self._out, self._in):
            try:
                if h:
                    h.close_port()
            except Exception:
                pass
        self._out = None
        self._in = None

    # ---- the transport ----
    def send_frame(self, frame):
        """One SysEx message. build_frame() already produces the packed wire
        frame, so it goes out as it is."""
        self._open()
        # rtmidi's send_message() takes ONE message as a flat list of bytes
        # (a list of lists is not what it wants).
        self._out.send_message(frame)

    def read_frames(self, seconds=1.0):
        """Whatever the device sends back, as raw SysEx messages."""
        name = self._open_input()
        if name is None:
            return []
        got = []
        self._in.ignore_types(sysex=False, timing=True, active_sense=True)

        def cb(msg, data, ts):
            got.append(bytes(data))
        self._in.set_callback(cb)
        import time
        time.sleep(seconds)
        self._in.cancel_callback()
        return got

    # ---- the commands ----
    def device_info(self, seconds=1.5):
        """A READ: the device's 27-byte info block. Safe to send."""
        self.send_frame(build_frame(CMD_DEVICE_INFO))
        replies = self.read_frames(seconds)
        if not replies:
            return None
        # the reply is F0 ... F7; strip it and the 00 59 header
        raw = replies[0]
        if raw[:1] == b"\xF0":
            raw = raw[1:]
        if raw[-1:] == b"\xF7":
            raw = raw[:-1]
        return raw

    def upload(self, blob, target=UPLOAD_NAM, slot=0, chunk=MAX_PAYLOAD,
               progress=None):
        """Upload a capture: arm the session, then stream the body.

        Returns the number of body bytes the device took.
        """
        if not blob:
            raise ProtocolError("there is nothing to upload")
        if len(blob) > 32768:
            raise ProtocolError("the session buffer holds 32768 bytes, this is %d"
                                % len(blob))
        header = bytes([len(blob) & 0xFF, (len(blob) >> 8) & 0xFF,
                        (len(blob) >> 16) & 0xFF, target & 0xFF, slot & 0xFF])
        self.send_frame(build_frame(CMD_WRITE_BEGIN, header))
        sent = 0
        while sent < len(blob):
            part = blob[sent:sent + chunk]
            self.send_frame(build_frame(CMD_WRITE_CHUNK, part))
            sent += len(part)
            if progress:
                progress(sent, len(blob))
        return sent


# --- the CLI ----------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--port", default=None, help="the MIDI output to use")
    ap.add_argument("--list", action="store_true", help="list the MIDI ports")
    ap.add_argument("--probe", action="store_true",
                    help="read the device info (safe - it is a read)")
    ap.add_argument("--upload", default=None, help="a .nam / .wav to upload")
    ap.add_argument("--target", type=int, default=UPLOAD_NAM,
                    help="1 = a NAM capture, 2 = a cabinet IR")
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--ramps", action="store_true",
                    help="print the frames instead of sending them")
    args = ap.parse_args(argv)

    if args.list:
        outs, ins = DeviceLink.ports()
        print("  the MIDI outputs:")
        for p in outs:
            print("   ", p)
        print("  the MIDI inputs:")
        for p in ins:
            print("   ", p)
        return 0

    if args.ramps:
        blob = b"\x00" * 32
        header = bytes([len(blob) & 0xFF, 0, 0, args.target, args.slot])
        frames = [build_frame(CMD_WRITE_BEGIN, header),
                  build_frame(CMD_WRITE_CHUNK, blob)]
        for f in frames:
            print("  ", f.hex())
        return 0

    link = DeviceLink(args.port)
    try:
        if args.probe:
            info = link.device_info()
            if info is None:
                print("  the device did not answer")
                return 1
            print("  the device answered %d bytes: %s" % (len(info), info.hex()))
            printable = "".join(chr(b) if 32 <= b < 127 else "." for b in info)
            print("  as text: %s" % printable)
            return 0

        if args.upload:
            with open(args.upload, "rb") as f:
                blob = f.read()
            print("  uploading %d bytes from %s" % (len(blob), args.upload))
            n = link.upload(blob, target=args.target, slot=args.slot,
                            progress=lambda s, t: print("    %d / %d" % (s, t)))
            print("  sent %d bytes" % n)
            return 0
    finally:
        link.close()

    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
