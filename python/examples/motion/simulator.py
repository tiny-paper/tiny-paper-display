#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fake motion for the demo: three rooms publish on the broker, each in another payload style (ON/OFF, JSON, true/false).
Topics match config.yml. Motion starts and stops at random; the last state is retained so the display has it at once."""
import json
import os
import random
import time

import paho.mqtt.client as mqtt

HOST, PORT = os.environ.get("MQTT_HOST", "mosquitto"), int(os.environ.get("MQTT_PORT", "1883"))
ROOMS = {  # topic: payload function
    "espectre/living/motion": lambda on: "ON" if on else "OFF",
    "espectre/kitchen/motion": lambda on: json.dumps({"motion": on, "movement": 0.8 if on else 0.02}),
    "espectre/bedroom/motion": lambda on: "true" if on else "false",
}


def main():
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        client = mqtt.Client()
    state = {t: False for t in ROOMS}
    nxt = {t: 0 for t in ROOMS}

    def publish(topic):
        client.publish(topic, ROOMS[topic](state[topic]), retain=True)

    def on_connect(c, userdata, flags, rc, *rest):
        # the broker may come up after this container: a publish made before the connection exists is dropped, so
        # every (re)connect sends the current state of every room again
        for topic in ROOMS:
            publish(topic)

    client.on_connect = on_connect
    client.reconnect_delay_set(1, 30)
    client.connect_async(HOST, PORT)
    client.loop_start()
    while True:
        now = time.time()
        for topic in ROOMS:
            if now >= nxt[topic]:
                state[topic] = not state[topic]
                publish(topic)
                nxt[topic] = now + random.uniform(20, 120 if state[topic] else 240)
        time.sleep(1)


if __name__ == "__main__":
    main()
