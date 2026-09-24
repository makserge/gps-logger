# Single Dockerfile used for BOTH deployment paths:
#   - plain `docker compose up -d --build` on the Pi
#   - Home Assistant Supervisor, when this whole `backend/` folder is copied
#     into HA's add-ons directory (Supervisor finds config.yaml here and
#     builds this Dockerfile as a local add-on)
#
# It installs fully offline from vendored wheels/ instead of `pip install
# -r requirements.txt` against PyPI. This is required for the Supervisor
# build path: the HA host's docker build network has proven unable to reach
# PyPI or the Alpine package CDN (only container registries work), so a
# plain `pip install` fails there. Vendoring the wheels makes both build
# paths work the same way, network or no network.
#
# Base is hardcoded (NOT `ARG BUILD_FROM`): Supervisor deprecates/ignores
# build.yaml and injects its own default BUILD_FROM
# (ghcr.io/home-assistant/base:latest - no Python), so an ARG-based FROM
# gets silently overridden there. python:3.12-alpine is multi-arch and
# needs no apk packages for this app.
#
# NOTE: the vendored wheels/ currently only include an aarch64 build of
# pydantic_core (see wheels/update-wheels.sh) - this image will fail to
# build on amd64 despite config.yaml listing amd64 support. Fine if you're
# only ever building on/for a Raspberry Pi; regenerate wheels/ for amd64
# too (see that script) if you need to build on an amd64 dev machine.
FROM python:3.12-alpine

WORKDIR /app

COPY app/requirements.txt .
COPY wheels/ /wheels/
RUN pip install --no-cache-dir --no-index --find-links=/wheels -r requirements.txt \
    && rm -rf /wheels

COPY app/main.py .
COPY app/static/ ./static/

# Default for when this runs as a plain `docker compose` service.
# docker-compose.yml overrides DATA_DIR explicitly for that path. When
# Supervisor runs this as an HA add-on instead, compose is never consulted,
# so this ENV default is what actually applies - /share/gpslogger/tracks
# matches the `map: - "share:rw"` mount declared in config.yaml.
ENV DATA_DIR=/share/gpslogger/tracks

VOLUME ["/data"]
EXPOSE 8080

CMD ["python", "-m", "uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8080"]
