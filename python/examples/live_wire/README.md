# Live wire

Feeds that push, shown the moment they happen: a page that moves while you watch, where e-paper usually refreshes every
5 to 15 minutes. One container, no extra hardware, no web server, no drivers, no accounts. Black and white.

| Box | Shows | Source |
|---|---|---|
| Wikipedia edits now | the newest edited titles of the chosen wikis, newest on top (inverted) | Wikimedia EventStreams `recentchange` (SSE push) |
| Edits per second | edits per second over all wikis, graph of the last 10 minutes, the most edited article in 10 minutes | same stream |
| Earthquakes 24 h | world map, one dot per quake sized by magnitude, the newest ringed, its magnitude, age and place in large text | USGS `all_day` GeoJSON, polled every 60 s (it is a feed, not push); optional EMSC websocket (push) |
| Bluesky (optional) | posts per second only, never the text | Jetstream websocket |

```
docker compose up -d          # from this folder; needs the display on /dev/ttyACM0
docker compose logs -f        # feed problems show here and as FEED DOWN in the box
```

Settings are environment lines in `docker-compose.yml`: `LIVE_WIRE_WIKIS` (default `enwiki,nlwiki`), `LIVE_WIRE_EMSC=1`,
`LIVE_WIRE_BLUESKY=1`, `LIVE_WIRE_CONTACT` (an e-mail or URL appended to the User-Agent, as Wikimedia's policy asks).
Without Docker: `pip install ./python aiohttp`, then `python python/examples/live_wire/live_wire.py`.

How it updates: the page is redrawn in memory whenever a shown number or title changed and sent with `Device.show` at most
once every 2 s; the device refreshes only the changed rectangle. A feed that fails shows FEED DOWN in its box and is
retried after 1 s, 2 s, 4 s ... up to 60 s; the page never stops. If the device unplugs, the port is reopened.

No device: `python live_wire.py --png page.png --demo` (fixed fake events), or `--png page.png --seconds 30 --png-every 10`
for the real feeds. `live-wire-demo-800x480.png` and `live-wire-demo-1872x1404.png` are the demo pictures.

Terms of the feeds (all free, no key):
| Feed | Terms |
|---|---|
| Wikimedia EventStreams | open; send a descriptive User-Agent (done); Wikipedia text is CC BY-SA, only titles are shown |
| USGS earthquakes | US public domain; keep polling at 60 s or slower |
| EMSC (SeismicPortal) | free use for non-commercial display with attribution: data from EMSC (emsc-csem.org) |
| Bluesky Jetstream | public firehose of public posts, counted only |

The coastline is Natural Earth 110m (public domain), simplified once by `gen_coast.py` into `coast_data.py` (7 KB).
Unproven until a person has looked: how it reads on the real panel, and ghosting over hours.
