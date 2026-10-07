#!/usr/bin/env python3
"""Fail when a firmware image links radio or network code (the display firmware: docs/usb-display.md "No radio").
check_no_radio.py <link map> [...]: reads the linker map's "Archive member included" list, so no toolchain is needed.
An archive on the list below with any member linked is a failure; so is a map without that list (a wrong file).
SPDX-License-Identifier: GPL-3.0-or-later"""
import re
import sys

# WiFi and Bluetooth stacks and their binary blobs, the radio PHY, TCP/IP, TLS sessions and HTTP
FORBIDDEN = {
    "libesp_wifi.a", "libnet80211.a", "libpp.a", "libcore.a", "libmesh.a", "libespnow.a", "libsmartconfig.a",
    "libwapi.a", "libwpa_supplicant.a", "libesp_coex.a", "libcoexist.a", "libphy.a", "libbtbb.a", "libesp_phy.a",
    "libbt.a", "libbtdm_app.a", "libble_app.a", "liblwip.a", "libesp_netif.a", "libesp_eth.a", "libesp-tls.a",
    "libesp_http_client.a", "libesp_http_server.a", "libesp_https_ota.a", "libtcp_transport.a", "libmbedtls.a",
    "libmbedx509.a", "libmqtt.a",
}
# Linked into every ESP-IDF image by the build itself (-u include_esp_phy_override): two stubs that keep the ADC and
# the PHY from sharing a register bus, no radio code (the PHY blob libphy.a and phy_init.c stay forbidden)
ALLOWED = {("libesp_phy.a", "phy_override.c.obj")}
MEMBER = re.compile(r"^(\S*?([^/\s]+\.a))\((\S+)\)\s*$")


def linked(text):
    """{archive name: [members]} of the map's "Archive member included" section; None when the map has none."""
    start = text.find("Archive member included")
    if start < 0:
        return None
    end = text.find("\nDiscarded input sections", start)
    if end < 0:
        end = text.find("\nMemory Configuration", start)
    out = {}
    for line in text[start:end if end > 0 else len(text)].splitlines():
        m = MEMBER.match(line)
        if m:
            if (m.group(2), m.group(3)) not in ALLOWED:
                out.setdefault(m.group(2), []).append(m.group(3))
    return out


def check(text):
    """(ok, lines)."""
    libs = linked(text)
    if not libs:
        return False, ["no archive list in the map: not a linker map, or an empty link"]
    bad = sorted(set(libs) & FORBIDDEN)
    if bad:
        return False, ["%s: %d members linked (%s ...)" % (a, len(libs[a]), ", ".join(libs[a][:3])) for a in bad]
    return True, ["%d archives linked, none of the %d radio and network ones" % (len(libs), len(FORBIDDEN))]


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
