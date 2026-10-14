# SPDX-License-Identifier: MIT
# The Bluetooth key: the key file, the handshake and sealed frames (docs/spec/usb-display.md "Bluetooth key").
"""Bluetooth without system pairing: the device makes a key over the USB cable (`tiny-paper-display setup --key`), the
host keeps it in a small file, and every frame over Bluetooth is sealed with AES-128-CCM under session keys from
HKDF-SHA256 (the `cryptography` package: pip install tiny-paper-display[bluetooth]). Vectors: docs/spec/vectors/
bt-key.json, shared with the firmware's tests.

A key file is JSON {"name", "kid", "key"} (hex), mode 0600, by default ~/.config/tiny-paper-display/keys/<name>.json
($XDG_CONFIG_HOME if set). Copy it to another computer (or into a container, then key_file=) to give it the device.
"""
import json
import os
import struct
from pathlib import Path

from . import codec
from .usb_display import DisplayError

KEY_LEN, KID_LEN, NONCE_LEN, TAG = 32, 8, 16, 16
H2D, D2H = 0x48, 0x44
BT_KEY, BT_AUTH = 0x0F, 0x10
INSTALL_HINT = "Bluetooth needs the cryptography library: pip install tiny-paper-display[bluetooth]"


class NoKey(DisplayError):
    """No key file for this device, or one the device does not take."""


class WrongKey(NoKey):
    """The device holds another key: its proof did not check."""


class LinkBroken(OSError):
    """A frame from the device did not open (changed, replayed, out of order, another key): the link is closed."""


def _crypto():
    try:
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.ciphers.aead import AESCCM
        from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    except ImportError as exc:
        raise DisplayError(INSTALL_HINT) from exc
    return hashes, AESCCM, HKDF


def require():
    """DisplayError with the install hint when `cryptography` is missing."""
    _crypto()


def _hkdf(ikm, salt, info, n):
    hashes, _, hkdf = _crypto()
    return hkdf(algorithm=hashes.SHA256(), length=n, salt=salt or None, info=info).derive(ikm)


def kid(key):
    """The key's id: 8 bytes."""
    return _hkdf(key, b"", b"tpd-bt-kid-1", KID_LEN)


def _nonce(direction, ctr):
    return bytes([direction]) + bytes(4) + struct.pack(">Q", ctr)


class Channel:
    """One direction pair of a session: seal what this side sends, open what it receives, each with its own counter."""

    def __init__(self, k_tx, d_tx, n_tx, k_rx, d_rx, n_rx):
        aesccm = _crypto()[1]
        self.tx, self.rx = aesccm(k_tx, tag_length=TAG), aesccm(k_rx, tag_length=TAG)
        self.d_tx, self.n_tx, self.d_rx, self.n_rx = d_tx, n_tx, d_rx, n_rx
        self.up = True

    def seal(self, seq, cmd, plain):
        if not self.up:
            raise LinkBroken("the Bluetooth session is over")
        out = self.tx.encrypt(_nonce(self.d_tx, self.n_tx), bytes(plain), bytes([seq, cmd]))
        self.n_tx += 1
        return out

    def open(self, seq, cmd, sealed):
        """The plain payload; LinkBroken (and the session over) when it does not open with the next counter."""
        from cryptography.exceptions import InvalidTag
        if not self.up:
            raise LinkBroken("the Bluetooth session is over")
        try:
            plain = self.rx.decrypt(_nonce(self.d_rx, self.n_rx), bytes(sealed), bytes([seq, cmd]))
        except (InvalidTag, ValueError) as exc:
            self.up = False
            raise LinkBroken("a frame from the device did not open (changed, replayed or another key): "
                             "the Bluetooth link is closed") from exc
        self.n_rx += 1
        return plain


def _session(key, host_nonce, device_nonce):
    keys = _hkdf(key, host_nonce + device_nonce, b"tpd-bt-session-1", 32)
    proof = _crypto()[1](keys[16:], tag_length=TAG).encrypt(
        _nonce(D2H, 0), b"", b"tpd-bt-auth-1" + kid(key) + host_nonce + device_nonce)
    return keys[:16], keys[16:], proof


def host_channel(key, host_nonce, device_nonce, proof):
    """The host's channel after the device's answer; WrongKey when its proof does not check."""
    import hmac
    h2d, d2h, want = _session(key, host_nonce, device_nonce)
    if not hmac.compare_digest(want, bytes(proof)):
        raise WrongKey("the device holds another key: make a new one over the cable (tiny-paper-display setup --key) "
                       "or use the key file of the computer that made it")
    return Channel(h2d, H2D, 0, d2h, D2H, 1)


def device_channel(key, host_nonce, device_nonce):
    """The device's side (the tests' fake firmware): (channel, proof)."""
    h2d, d2h, proof = _session(key, host_nonce, device_nonce)
    return Channel(d2h, D2H, 1, h2d, H2D, 0), proof


# ---------------------------------------------------------------- key files

def key_dir():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return Path(base) / "tiny-paper-display" / "keys"


def save(name, kid_, key, path=None):
    """Write the key file (mode 0600, the folder 0700); returns its path."""
    path = Path(path) if path else key_dir() / f"{name}.json"
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    text = json.dumps({"name": name, "kid": bytes(kid_).hex(), "key": bytes(key).hex()}, indent=1) + "\n"
    tmp = path.with_name(path.name + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text)
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)
    return path


def load(name=None, key_file=None):
    """{"name", "kid", "key"} from key_file, else from the key folder by the device's name. NoKey when there is none."""
    path = Path(key_file) if key_file else key_dir() / f"{name}.json" if name else None
    if path is None or not path.is_file():
        where = path or key_dir()
        raise NoKey(f"no Bluetooth key for {name or 'this device'} ({where}): make one over the USB cable with "
                    "tiny-paper-display setup --key, or give key_file=")
    try:
        data = json.loads(path.read_text())
        out = {"name": data.get("name", ""), "kid": bytes.fromhex(data["kid"]), "key": bytes.fromhex(data["key"])}
    except (ValueError, KeyError, TypeError) as exc:
        raise NoKey(f"{path} is not a key file: {exc}") from exc
    if len(out["key"]) != KEY_LEN or out["kid"] != kid(out["key"]):
        raise NoKey(f"{path} is damaged: the key and its id do not match")
    return out


# ---------------------------------------------------------------- the sealed port

class SealedPort:
    """A port over a Bluetooth port (ble_port.BlePort) that does the handshake with `key` and seals every frame both
    ways; read and write carry plain frames, as the serial port does. The same request written twice in a row (the
    link client's retry) goes out as the same sealed bytes."""

    is_ble = True

    def __init__(self, raw, key, timeout=5):
        self.raw, self.parser, self.out = raw, codec.LinkParser(), bytearray()
        self.last = (None, None)
        self.channel = self._handshake(key, timeout)

    def __getattr__(self, name):  # timeout, write_timeout, mtu...: the raw port's
        return getattr(self.raw, name)

    def __setattr__(self, name, value):
        if name in ("timeout", "write_timeout") and "raw" in self.__dict__:
            setattr(self.raw, name, value)
        else:
            super().__setattr__(name, value)

    def _handshake(self, key, timeout):
        import time
        hn = os.urandom(NONCE_LEN)
        self.raw.write(codec.build_frame(1, BT_AUTH, {"kid": kid(key), "nonce": hn}))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for item in self.parser.feed(self.raw.read(4096)):
                if item[0] != "frame" or item[1] != 1 or item[2] != BT_AUTH | 0x80:
                    continue
                reply = codec.cbor_decode(item[3]) if item[3] else {}
                if "err" in reply:
                    raise NoKey(f"the device refused the key: {reply.get('msg', '')} (make one over the USB cable "
                                "with tiny-paper-display setup --key)")
                dn, proof = reply.get("nonce"), reply.get("proof")
                if not isinstance(dn, bytes) or len(dn) != NONCE_LEN or not isinstance(proof, bytes):
                    raise DisplayError(f"invalid handshake answer: {reply}")
                return host_channel(key, hn, dn, proof)
        raise DisplayError("the device did not answer the Bluetooth handshake")

    def write(self, data):
        data = bytes(data)
        if data == self.last[0]:
            sealed = self.last[1]  # a retry: the same bytes, never sealed anew
        else:
            sealed = bytearray()
            for item in codec.LinkParser().feed(data):
                if item[0] == "frame":
                    _, seq, cmd, payload = item
                    sealed += codec.build_frame(seq, cmd, self.channel.seal(seq, cmd, payload))
            sealed = bytes(sealed)
            self.last = (data, sealed)
        self.raw.write(sealed)
        return len(data)

    def read(self, size):
        if not self.out:
            for item in self.parser.feed(self.raw.read(4096)):
                if item[0] == "frame":
                    _, seq, cmd, payload = item
                    try:
                        plain = self.channel.open(seq, cmd, payload)
                    except LinkBroken:
                        self.close()
                        raise
                    self.out += codec.build_frame(seq, cmd, plain)
        out = bytes(self.out[:size])
        del self.out[:size]
        return out

    def close(self):
        self.raw.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
