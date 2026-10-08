#!/usr/bin/env python3
"""Fail when the display firmware links WiFi or network code, or reaches Bluetooth by another way than its gate
(docs/spec/usb-display.md "Bluetooth": off by default, started only behind the stored setting).
check_no_radio.py <link map> [...]: reads the linker map, so no toolchain is needed. What a pass proves:

1. No WiFi and no network: no member of a FORBIDDEN archive is linked (WiFi stack and blobs, TCP/IP, TLS, HTTP).
2. Bluetooth only through the gate: a member of a BLUETOOTH archive (the NimBLE host, the controller blob, the radio
   PHY and coexistence blobs) is in the image only because the gate (hal_esp's ble_link.c) or other Bluetooth code
   referenced it ("Archive member included"), and no object outside those references a symbol the Bluetooth host or
   controller defines ("Cross Reference Table"). So the stack's entry points have one caller in the image.

What it cannot prove: that the gate is called only with the setting on (components/usbdisp/test/test_usbdisp_bt.c
counts the calls; test_check_no_radio.py checks that one source opens the link), and what the radio does on a device.
A map without the archive list, or with Bluetooth linked but without the cross reference table, fails (a wrong file).
SPDX-License-Identifier: GPL-3.0-or-later"""
import re
import sys

# WiFi stack and its binary blobs, TCP/IP, TLS sessions and HTTP: never in the image
FORBIDDEN = {
    "libesp_wifi.a", "libnet80211.a", "libpp.a", "libcore.a", "libmesh.a", "libespnow.a", "libsmartconfig.a",
    "libwapi.a", "libwpa_supplicant.a", "liblwip.a", "libesp_netif.a", "libesp_eth.a", "libesp-tls.a",
    "libesp_http_client.a", "libesp_http_server.a", "libesp_https_ota.a", "libtcp_transport.a", "libmbedtls.a",
    "libmbedx509.a", "libmqtt.a",
}
# The Bluetooth host and controller: nothing outside them and the gate may reference a symbol they define
BT_STACK = {"libbt.a", "libbtdm_app.a", "libble_app.a", "libbtbb.a"}
# With them the radio PHY and coexistence blobs, which the controller pulls in: linked only for Bluetooth
BLUETOOTH = BT_STACK | {"libphy.a", "libesp_phy.a", "libesp_coex.a", "libcoexist.a"}
# The one object of our code that may reach Bluetooth
GATE = ("libhal_esp.a", "ble_link.c.obj")
# Linked into every ESP-IDF image by the build itself (-u include_esp_phy_override): two stubs that keep the ADC and
# the PHY from sharing a register bus, no radio code (the PHY blob libphy.a and phy_init.c stay behind the gate)
ALLOWED = {("libesp_phy.a", "phy_override.c.obj")}
MEMBER = re.compile(r"^(?:\S*/)?([^/\s(]+\.a)\(([^()\s]+)\)")
FILE = re.compile(r"(?:\S*/)?([^/\s(]+\.a)\(([^()\s]+)\)\s*$")


def section(text, title, ends):
    start = text.find(title)
    if start < 0:
        return None
    stops = [e for e in (text.find(end, start) for end in ends) if e > 0]
    return text[start:min(stops) if stops else len(text)]


def members(text):
    """[((archive, member), referrer (archive, member) or None)] of the map's "Archive member included" section;
    None when the map has none. The referrer is on the same line or the next."""
    part = section(text, "Archive member included", ("\nDiscarded input sections", "\nMemory Configuration"))
    if part is None:
        return None
    out, lines = [], part.splitlines()
    for i, line in enumerate(lines):
        m = MEMBER.match(line)
        if not m:
            continue
        rest = line[m.end():].strip() or (lines[i + 1].strip() if i + 1 < len(lines) else "")
        r = MEMBER.match(rest)
        out.append(((m.group(1), m.group(2)), (r.group(1), r.group(2)) if r else None))
    return out


def linked(text):
    """{archive name: [members]} without the ALLOWED ones; None when the map has no archive list."""
    got = members(text)
    if got is None:
        return None
    out = {}
    for who, _ in got:
        if who not in ALLOWED:
            out.setdefault(who[0], []).append(who[1])
    return out


def crossrefs(text):
    """[(symbol, defining (archive, member) or None, [user (archive, member)])] of the "Cross Reference Table"; None
    without one. Objects that are in no archive are users named (None, path)."""
    part = section(text, "Cross Reference Table", ())
    if part is None:
        return None
    out = []
    for line in part.splitlines()[1:]:
        if not line.strip() or line.startswith("Symbol"):
            continue
        m = FILE.search(line)
        who = (m.group(1), m.group(2)) if m else (None, line.split()[-1])
        if line[0].isspace():
            if out:
                out[-1][2].append(who)
        else:
            out.append((line.split()[0], who if m else None, []))
    return out


def script_symbols(text):
    """Names the linker script gives an address (ROM functions and data): the cross reference table lists their
    users only, the first of which is not a definition."""
    part = section(text, "Linker script and memory map", ("\nCross Reference Table",)) or ""
    return set(re.findall(r"^\s+0x[0-9a-f]+\s+(?:PROVIDE \()?(\w+) = ", part, re.M))


def inside(who):
    return who is not None and (who[0] in BLUETOOTH or who == GATE or who in ALLOWED)


def check(text):
    """(ok, lines)."""
    got = members(text)
    libs = linked(text)
    if not libs:
        return False, ["no archive list in the map: not a linker map, or an empty link"]
    bad = sorted(set(libs) & FORBIDDEN)
    if bad:
        return False, ["%s: %d members linked (%s ...)" % (a, len(libs[a]), ", ".join(libs[a][:3])) for a in bad]
    bt = [(who, ref) for who, ref in got if who[0] in BLUETOOTH and who not in ALLOWED]
    if not bt:
        return True, ["%d archives linked, none of the %d WiFi and network ones, no Bluetooth" % (len(libs), len(FORBIDDEN))]
    # Bluetooth is linked: only through the gate
    fails = ["%s(%s) is linked for %s, not for the gate %s(%s) or other Bluetooth code"
             % (who[0], who[1], "%s(%s)" % ref if ref else "the build itself", GATE[0], GATE[1])
             for who, ref in bt if not inside(ref)]
    if not any(who == GATE for who, _ in got):
        fails.append("Bluetooth is linked but the gate %s(%s) is not" % GATE)
    refs = crossrefs(text)
    if refs is None:
        fails.append("Bluetooth is linked but the map has no cross reference table (link with --cref)")
    reached, rom = 0, script_symbols(text)
    for sym, definer, users in refs or []:
        if definer is None or definer[0] not in BT_STACK or sym in rom:
            continue
        reached += 1
        fails += ["%s of %s(%s) is referenced by %s(%s), outside the gate" % (sym, definer[0], definer[1], u[0], u[1])
                  for u in users if not inside(u)]
    if refs is not None and not reached:
        fails.append("Bluetooth is linked but the cross reference table names none of its symbols")
    if fails:
        return False, fails[:20] + (["... %d more" % (len(fails) - 20)] if len(fails) > 20 else [])
    return True, ["%d archives linked, none of the %d WiFi and network ones; Bluetooth (%d members, %d symbols) only "
                  "through %s(%s)" % (len(libs), len(FORBIDDEN), len(bt), reached, GATE[0], GATE[1])]


def main(argv):
    if not argv:
        sys.exit(__doc__)
    rc = 0
    for path in argv:
        ok, lines = check(open(path, errors="replace").read())
        for line in lines:
            print("check_no_radio %s: %s%s" % (path, "" if ok else "FAIL ", line))
        rc |= not ok
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
