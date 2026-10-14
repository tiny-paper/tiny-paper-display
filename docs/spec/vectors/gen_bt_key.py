#!/usr/bin/env python3
"""Vectors of the Bluetooth key (docs/spec/usb-display.md "Bluetooth key"): bt-key.json, read by
firmware/components/btseal/test/test_btseal.c, firmware/components/usbdisp/test/test_usbdisp_btkey.c and
python/tests/test_bt_key.py. Needs the `cryptography` package (a venv, never system-wide).
Run: python3 docs/spec/vectors/gen_bt_key.py [--check]"""
import hashlib
import json
import os
import struct
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_usb_display import BT, BT_NAME, HAVE, cbor, frame  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bt-key.json")
BT_KEY, BT_AUTH = 0x0F, 0x10
H2D, D2H = 0x48, 0x44
CABLE_MSG = "bt and bt_key work only over the USB cable"


def fixed(label, n):
    """Deterministic test bytes (never a real key)."""
    return hashlib.sha256(b"tpd vector " + label.encode()).digest()[:n]


def hkdf(ikm, salt, info, n):
    return HKDF(algorithm=hashes.SHA256(), length=n, salt=salt or None, info=info).derive(ikm)


def nonce(direction, ctr):
    return bytes([direction]) + bytes(4) + struct.pack(">Q", ctr)


def seal(key, direction, ctr, seq, cmd, plain):
    return AESCCM(key, tag_length=16).encrypt(nonce(direction, ctr), plain, bytes([seq, cmd]))


def build():
    key, other = fixed("key", 32), fixed("other key", 32)
    kid = hkdf(key, b"", b"tpd-bt-kid-1", 8)
    hn, dn = fixed("host nonce", 16), fixed("device nonce", 16)
    sk = hkdf(key, hn + dn, b"tpd-bt-session-1", 32)
    k_h2d, k_d2h = sk[:16], sk[16:]
    proof = AESCCM(k_d2h, tag_length=16).encrypt(nonce(D2H, 0), b"", b"tpd-bt-auth-1" + kid + hn + dn)

    def sealed(k, direction, ctr, seq, cmd, obj):
        return frame(seq, cmd, seal(k, direction, ctr, seq, cmd, cbor(obj)))

    have = {"fingerprint": "0000000000000000"}
    frames = [  # in order on one connection after the handshake
        {"name": "have", "dir": "h2d", "ctr": 0, "seq": 2, "cmd": HAVE, "plain": cbor(have).hex(),
         "wire": sealed(k_h2d, H2D, 0, 2, HAVE, have).hex()},
        {"name": "have_answer", "dir": "d2h", "ctr": 1, "seq": 2, "cmd": HAVE | 0x80, "plain": cbor({"send": True}).hex(),
         "wire": sealed(k_d2h, D2H, 1, 2, HAVE | 0x80, {"send": True}).hex()},
        {"name": "bt", "dir": "h2d", "ctr": 1, "seq": 3, "cmd": BT, "plain": cbor({}).hex(),
         "wire": sealed(k_h2d, H2D, 1, 3, BT, {}).hex()},
        {"name": "bt_answer", "dir": "d2h", "ctr": 2, "seq": 3, "cmd": BT | 0x80,
         "plain": cbor({"err": 38, "msg": CABLE_MSG}).hex(),
         "wire": sealed(k_d2h, D2H, 2, 3, BT | 0x80, {"err": 38, "msg": CABLE_MSG}).hex()},
    ]
    good = bytes.fromhex(frames[2]["wire"])  # what the device expects third: h2d counter 1
    forged = bytearray(good)
    forged[-3] ^= 1  # the tag's last byte (before the CRC); CRC made right again below
    forged = frame(3, BT, bytes(forged[6:-2]))
    refused = [  # each sent where frames[2] is due (h2d counter 1): the device drops the link without an answer
        {"name": "forged_tag", "wire": forged.hex()},
        {"name": "replayed_counter", "wire": sealed(k_h2d, H2D, 0, 3, BT, {}).hex()},
        {"name": "wrong_key", "wire": sealed(hkdf(other, hn + dn, b"tpd-bt-session-1", 32)[:16], H2D, 1, 3, BT, {}).hex()},
        {"name": "swapped_direction", "wire": sealed(k_d2h, D2H, 1, 3, BT, {}).hex()},
        {"name": "plain_frame", "wire": frame(3, BT, cbor({})).hex()},
    ]
    return {
        "about": "Bluetooth key (usb-display.md \"Bluetooth key\"). bt_key: the cable answer when the device's RNG gives "
                 "`key`. auth: the handshake on Bluetooth with the device's random nonce `device_nonce`. frames: sealed, "
                 "in order after it. refused: each in place of frames[2]; the link is dropped, nothing answered. "
                 "Test bytes only, never a real key.",
        "name": BT_NAME,
        "key": key.hex(), "kid": kid.hex(), "host_nonce": hn.hex(), "device_nonce": dn.hex(),
        "key_h2d": k_h2d.hex(), "key_d2h": k_d2h.hex(), "proof": proof.hex(),
        "bt_key": {"tx": frame(1, BT_KEY, cbor({})).hex(),
                   "rx": frame(1, BT_KEY | 0x80, cbor({"on": True, "name": BT_NAME, "kid": kid, "key": key})).hex()},
        "auth": {"tx": frame(1, BT_AUTH, cbor({"kid": kid, "nonce": hn})).hex(),
                 "rx": frame(1, BT_AUTH | 0x80, cbor({"nonce": dn, "proof": proof})).hex()},
        "frames": frames,
        "refused": refused,
    }


def main():
    text = json.dumps(build(), indent=1) + "\n"
    if "--check" in sys.argv:
        if open(OUT).read() != text:
            sys.exit("bt-key.json is stale: run gen_bt_key.py")
        return
    open(OUT, "w").write(text)


if __name__ == "__main__":
    main()
