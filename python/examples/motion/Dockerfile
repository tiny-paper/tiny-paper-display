# Build from the repository root:  docker build -f python/examples/motion/Dockerfile -t motion-display .
# python:3.12-slim is multi-arch (amd64, arm64 for a Raspberry Pi 4); pure Python only, no compiler needed.
FROM python:3.12-slim
WORKDIR /app
COPY python /src/python
RUN pip install --no-cache-dir /src/python "paho-mqtt>=2,<3" "pyyaml>=6"
COPY python/examples/motion/motion_display.py python/examples/motion/simulator.py python/examples/motion/config.yml /app/
CMD ["python", "-u", "motion_display.py"]
