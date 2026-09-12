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

ENV NODE_ENV=production \
    HTTP_PORT=8788 \
    TCP_PORT=8787 \
    STATIC_DIR=/app/public

COPY --from=serverbuild /app/server/dist         ./dist
COPY --from=serverbuild /app/server/node_modules ./node_modules
COPY --from=webbuild    /app/web/dist            ./public

# 8788: HTTP + WebSocket, what CapRover's nginx routes pong.wardcrew.com to.
# 8787: raw TCP for the 3DS on the LAN -- needs a port mapping in the app config,
#       since nginx cannot proxy a non-HTTP protocol.
EXPOSE 8788 8787

USER node

HEALTHCHECK --interval=15s --timeout=3s --start-period=10s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8788/healthz >/dev/null 2>&1 || exit 1

CMD ["node", "dist/server/src/index.js"]
