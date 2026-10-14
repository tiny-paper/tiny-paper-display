# SPDX-License-Identifier: MIT
"""The Bluetooth key (docs/spec/usb-display.md "Bluetooth key"): the vectors of bt-key.json (the firmware's host tests
read the same file), the key file, and the host's refusals of every bad answer: a forged tag, a replayed counter,
another key, the other direction."""
import json
import os
import stat
import sys

import pytest

from tiny_paper_display import bt_key
from tiny_paper_display import display_fw as fw
from tiny_paper_display.usb_display import ROOT, Disconnected, Display, DisplayError, codec, open_port
from conftest import FakePort
from fake_ble import Air, install
from fake_display_fw import FakeDevice

K = json.loads((ROOT / "docs/spec/vectors/bt-key.json").read_text())
B = {k: bytes.fromhex(K[k]) for k in ("key", "kid", "host_nonce", "device_nonce", "proof")}
NAME = K["name"]


def frame_parts(hexwire):
    w = bytes.fromhex(hexwire)
    return w[4], w[5], w[6:-2]


# ---------------------------------------------------------------- vectors

def test_kid_proof_and_frames_both_ways():
    assert bt_key.kid(B["key"]) == B["kid"]
    dev, proof = bt_key.device_channel(B["key"], B["host_nonce"], B["device_nonce"])
    assert proof == B["proof"]
    host = bt_key.host_channel(B["key"], B["host_nonce"], B["device_nonce"], proof)
    for f in K["frames"]:
        sender, receiver = (host, dev) if f["dir"] == "h2d" else (dev, host)
        seq, cmd, sealed = frame_parts(f["wire"])
        plain = bytes.fromhex(f["plain"])
        assert codec.build_frame(seq, cmd, sender.seal(seq, cmd, plain)).hex() == f["wire"], f["name"]
        assert receiver.open(seq, cmd, sealed) == plain, f["name"]


@pytest.mark.parametrize("bad", K["refused"], ids=[r["name"] for r in K["refused"]])
def test_each_refused_frame_ends_the_session(bad):
    dev, _ = bt_key.device_channel(B["key"], B["host_nonce"], B["device_nonce"])
    dev.open(*frame_parts(K["frames"][0]["wire"]))
    with pytest.raises(bt_key.LinkBroken):
        dev.open(*frame_parts(bad["wire"]))
    with pytest.raises(bt_key.LinkBroken):  # over: the good frame no longer opens either
        dev.open(*frame_parts(K["frames"][2]["wire"]))


def test_fake_firmware_answers_the_vectors_byte_for_byte():
    """The fake firmware the other tests talk to does what the vectors say: bt_key, the handshake, sealed frames."""
    dev = FakeDevice(bt="off", name=NAME)
    dev.next_random = [B["key"], B["device_nonce"]]
    assert dev.respond(bytes.fromhex(K["bt_key"]["tx"])).hex() == K["bt_key"]["rx"]
    dev.new_connection()
    assert dev.respond(bytes.fromhex(K["auth"]["tx"]), link="ble").hex() == K["auth"]["rx"]
    for q, a in zip(K["frames"][::2], K["frames"][1::2]):
        assert dev.respond(bytes.fromhex(q["wire"]), link="ble").hex() == a["wire"], q["name"]
    assert dev.broken == 0


def test_the_host_sends_the_vectors_handshake(monkeypatch):
    sent = []

    class Raw:
        def write(self, data):
            sent.append(bytes(data))
            return len(data)

        def read(self, _size):
            return bytes.fromhex(K["auth"]["rx"])

        def close(self):
            pass

    monkeypatch.setattr(bt_key.os, "urandom", lambda n: B["host_nonce"])
    port = bt_key.SealedPort(Raw(), B["key"])
    assert sent == [bytes.fromhex(K["auth"]["tx"])]
    seq, cmd, _ = frame_parts(K["frames"][0]["wire"])
    port.write(codec.build_frame(seq, cmd, bytes.fromhex(K["frames"][0]["plain"])))
    assert sent[-1].hex() == K["frames"][0]["wire"]


# ---------------------------------------------------------------- the key file

def test_key_file_is_private_and_loads_by_name(key_folder):
    path = bt_key.save(NAME, B["kid"], B["key"])
    assert path == key_folder / f"{NAME}.json"
    assert stat.S_IMODE(os.stat(path).st_mode) == 0o600 and stat.S_IMODE(os.stat(key_folder).st_mode) == 0o700
    assert bt_key.load(NAME) == {"name": NAME, "kid": B["kid"], "key": B["key"]}
    other = bt_key.save(NAME, B["kid"], B["key"], key_folder.parent / "copy.json")
    assert bt_key.load("whatever", key_file=other)["key"] == B["key"]  # key_file= wins (a container)


def test_missing_or_damaged_key_file(key_folder):
    with pytest.raises(bt_key.NoKey, match="setup --key"):
        bt_key.load(NAME)
    path = bt_key.save(NAME, B["kid"], B["key"])
    data = json.loads(path.read_text())
    path.write_text(json.dumps({**data, "kid": "00" * 8}))
    with pytest.raises(bt_key.NoKey, match="damaged"):
        bt_key.load(NAME)
    path.write_text("{")
    with pytest.raises(bt_key.NoKey, match="not a key file"):
        bt_key.load(NAME)


def test_without_cryptography_the_error_says_how_to_install_it(monkeypatch):
    for mod in [m for m in sys.modules if m.startswith("cryptography")]:
        monkeypatch.delitem(sys.modules, mod)
    monkeypatch.setitem(sys.modules, "cryptography", None)
    with pytest.raises(DisplayError, match=r"pip install tiny-paper-display\[bluetooth\]"):
        bt_key.kid(B["key"])


# ---------------------------------------------------------------- over the air

def setup_device(monkeypatch, **air):
    """A device with Bluetooth on and its key made over the cable, the key saved as setup does."""
    dev = FakeDevice(64, 64, (1, 2), (1,), True, 20, "vec_gray", "0.0.0+vec", bt="off", name=NAME)
    answer, path = fw.bt_key(Display(FakePort(dev.respond)))
    assert answer["on"] is True and "key" not in answer and dev.key and path.exists()
    return dev, install(monkeypatch, Air(dev, name=NAME, **air))


def test_key_made_over_the_cable_opens_bluetooth(monkeypatch):
    dev, air = setup_device(monkeypatch)
    with open_port(bluetooth=NAME) as port:
        hello = Display(port).hello()
    assert hello["bt"] == "on" and dev.broken == 0 and dev.auth_refused == 0
    assert air.writes[0][0][5] == bt_key.BT_AUTH  # the handshake first
    assert all(b"display" not in p for p, _ in air.writes[1:])  # no plain CBOR on the air after it


def tampered(monkeypatch, change):
    dev, air = setup_device(monkeypatch)
    answers = []

    def tamper(answer):
        answers.append(answer)
        return change(answer, answers) if len(answers) > 2 else answer  # the handshake and a hello go through

    air.tamper = tamper
    port = open_port(bluetooth=NAME)
    display = Display(port)
    display.hello()
    return port, display, dev


def reframe(answer, payload):
    return codec.build_frame(answer[4], answer[5], payload)


def other_direction(dev):
    """A frame sealed with the session's host-to-device key and direction (a reflected request), as the device's 3rd."""
    return lambda a, _: reframe(a, dev.channel.rx.encrypt(bt_key._nonce(bt_key.H2D, 2), b"\xa0", bytes(a[4:6])))


@pytest.mark.parametrize("bad", ["forged_tag", "replayed_counter", "other_direction"])
def test_host_refuses_a_bad_answer_and_closes(monkeypatch, bad):
    holder = {}
    changes = {"forged_tag": lambda a, _: reframe(a, a[6:-3] + bytes([a[-3] ^ 1])),
               "replayed_counter": lambda a, seen: seen[1],  # the hello answer again
               "other_direction": lambda a, seen: other_direction(holder["dev"])(a, seen)}
    port, display, dev = tampered(monkeypatch, changes[bad])
    holder["dev"] = dev
    with pytest.raises(Disconnected, match="did not open"):
        display.hello()
    assert not port.channel.up
    with pytest.raises(bt_key.LinkBroken):
        port.write(codec.build_frame(9, 1, {}))  # the session is over


def test_device_with_another_key(monkeypatch, key_folder):
    dev, air = setup_device(monkeypatch)
    dev.key = os.urandom(32)  # a new key made by someone else: our kid is not its
    with pytest.raises(bt_key.NoKey, match="refused the key"):
        open_port(bluetooth=NAME)
    assert dev.auth_refused == 1 and air.gone


def test_wrong_proof_is_wrong_key(monkeypatch):
    dev, air = setup_device(monkeypatch)
    air.tamper = lambda a: reframe(a, codec.cbor_encode({"nonce": b"\1" * 16, "proof": b"\2" * 16}))
    with pytest.raises(bt_key.WrongKey, match="another key"):
        open_port(bluetooth=NAME)
