# Multi-stage build for the Pong3DS server + web client.
#
# CapRover builds this ON THE HOST (x86_64), which conveniently sidesteps the
# arm64/x86_64 mismatch with the Mac this was developed on.
#
# The 3DS client is deliberately absent: it needs devkitARM, is not part of the
# server image, and is excluded via .dockerignore so a toolchain change can
# never invalidate this build cache.

# ---------- shared protocol codegen -----------------------------------------
FROM node:22-alpine AS proto
WORKDIR /app
COPY shared/ ./shared/
COPY tools/ ./tools/
# Regenerate rather than trusting the committed output, so a stale generated
# file cannot ship. `make verify` enforces the same thing in CI.
RUN node tools/gen-protocol.mjs

# ---------- web client -------------------------------------------------------
FROM node:22-alpine AS webbuild
WORKDIR /app
# The build number has to be declared HERE as well as in the runtime stage.
# Docker ARGs are scoped per stage, and the browser bundle bakes it in at build
# time -- without this the page would report 0 while /healthz reported the real
# number, which is worse than showing nothing.
ARG BUILD_ID=0
ENV BUILD_ID=${BUILD_ID}
COPY web/package*.json ./web/
RUN cd web && npm ci --no-audit --no-fund
COPY --from=proto /app/shared ./shared
COPY web/ ./web/
RUN cd web && npm run build

# ---------- server -----------------------------------------------------------
FROM node:22-alpine AS serverbuild
WORKDIR /app
COPY server/package*.json ./server/
RUN cd server && npm ci --no-audit --no-fund
COPY --from=proto /app/shared ./shared
COPY server/ ./server/
RUN cd server && npm run build && npm prune --omit=dev

# ---------- runtime ----------------------------------------------------------
FROM node:22-alpine AS runtime
WORKDIR /app

# Monotonic build number, surfaced at /api/version and compared by the 3DS
# auto-updater. CI passes the workflow run number; a local build gets 0, which
# no published build will ever equal.
ARG BUILD_ID=0
ARG GIT_SHA=local
# Where a human can get this build. A .cia cannot install another .cia (that
# needs am:u, which is FBI's job), so the updater shows this instead of
# pretending it can update itself.
ARG RELEASE_URL=

ENV NODE_ENV=production \
    HTTP_PORT=8788 \
    TCP_PORT=8787 \
    STATIC_DIR=/app/public \
    BUILD_ID=${BUILD_ID} \
    GIT_SHA=${GIT_SHA} \
    RELEASE_URL=${RELEASE_URL}

COPY --from=serverbuild /app/server/dist         ./dist
COPY --from=serverbuild /app/server/node_modules ./node_modules
COPY --from=webbuild    /app/web/dist            ./public

# The 3DS build that matches this server build.
#
# CI places the .3dsx/.cia here before building the image, so a container always
# serves the console build with the same BUILD_ID it reports at /api/version.
# That is what makes the in-app updater work straight from CI: there is no
# separate upload step to forget, and no way for the two to drift apart.
# Empty in a local build, which simply means no update is offered.
COPY deploy/downloads/ ./public/downloads/

# 8788: HTTP + WebSocket, what CapRover's nginx routes pong.wardcrew.com to.
# 8787: raw TCP for the 3DS on the LAN -- needs a port mapping in the app config,
#       since nginx cannot proxy a non-HTTP protocol.
EXPOSE 8788 8787

USER node

HEALTHCHECK --interval=15s --timeout=3s --start-period=10s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8788/healthz >/dev/null 2>&1 || exit 1

CMD ["node", "dist/server/src/index.js"]
