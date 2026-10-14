# Build from the repository root:  docker build -f python/examples/live_wire/Dockerfile -t live-wire .
# python:3.12-slim is multi-arch (amd64, arm64 for a Raspberry Pi); aiohttp ships wheels for both, no compiler needed.
FROM python:3.12-slim
WORKDIR /app
COPY python /src/python
RUN pip install --no-cache-dir /src/python "aiohttp>=3.9,<4"
COPY python/examples/live_wire/live_wire.py python/examples/live_wire/coast_data.py /app/
CMD ["python", "-u", "live_wire.py"]
