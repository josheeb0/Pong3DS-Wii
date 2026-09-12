# Deploying Pong3DS

The server is one container exposing two ports:

| Port | Protocol | Who uses it |
|------|----------|-------------|
| 8788 | HTTP + WebSocket + SSE | the browser client, and the 3DS when it is away from home |
| 8787 | raw TCP | the 3DS on the LAN (fast path, full 60Hz) |

8787 cannot go through nginx or the Cloudflare tunnel — it is not HTTP. It is
published on the host so the 3DS can reach `192.168.4.29:8787` directly.

---

## Deploying from GHCR (preferred)

CI publishes `ghcr.io/johndoe6345789/pong3ds` on every push to `main`. The host
pulls a tested image rather than building from synced source, which means the
thing running in production is the exact artifact CI verified.

The package is public (like the other `ghcr.io/johndoe6345789/*` images this
host already pulls), so no `docker login` is needed on the host.

```bash
ssh r@rdesktop.local '
  docker pull ghcr.io/johndoe6345789/pong3ds:latest
  docker rm -f pong-server 2>/dev/null || true
  docker run -d --name pong-server --restart unless-stopped \
    --network captain-overlay-network \
    -p 8788:8788 -p 8787:8787 \
    -e PUBLIC_ORIGIN=https://pong.wardcrew.com \
    --memory 512m --cpus 1.0 \
    ghcr.io/johndoe6345789/pong3ds:latest
'
```

Confirm which build is actually live — this is how you catch a deploy that
silently did not take:

```bash
curl -s http://192.168.4.29:8788/healthz
# {"ok":true,"protocol":1,"build":42,"sha":"abc123...","uptimeMs":...}
```

`build` is the CI run number and `sha` the commit, both baked in at image build
time. If they do not match the run you expected, the pull did not happen.

Pin a specific build instead of `latest` by using its tag:
`ghcr.io/johndoe6345789/pong3ds:sha-abc1234`.

### Or use compose

`compose.yml` at the repo root does the same thing and is easier to keep
correct, since the ports, environment and health check live in a file rather
than in a shell command someone has to retype:

```bash
docker compose pull && docker compose up -d
```

Note that `docker compose up` genuinely replaces the container, which
`docker restart` does not -- restarting reuses the old image and produces a
deploy that appears to work and changes nothing. Check `/healthz` afterwards
either way.

### The 3DS build travels inside the image

CI builds the console client and copies it into the image before publishing, so
`/downloads/pong3ds.3dsx` and `/downloads/pong3ds.cia` always match the
`BUILD_ID` the server reports. Pulling a new image therefore updates what the
in-app updater offers, with no separate upload step to forget and no way for the
two to drift apart.

There is no bind mount any more. If you want to override what is served (a local
test build, say), mount over it explicitly:

```bash
-v $HOME/pong3ds-downloads:/app/public/downloads:ro
```

Check what a console would be offered:

```bash
curl -s https://pong.wardcrew.com/api/version
```

## Building on the host (fallback)

Useful when iterating faster than CI, or when GitHub is unreachable. Built and
run directly with Docker on `rdesktop.local`, joined to
`captain-overlay-network` so cloudflared can reach it by container name:

```bash
rsync -az --delete \
  --exclude node_modules --exclude dist --exclude .git \
  --exclude '3ds/vendor/.src' --exclude '3ds/vendor/mbedtls' \
  ./ r@rdesktop.local:~/pong3ds/

ssh r@rdesktop.local '
  cd ~/pong3ds
  docker build -f deploy/Dockerfile -t pong3ds:latest .
  docker rm -f pong-server 2>/dev/null || true
  docker run -d --name pong-server --restart unless-stopped \
    --network captain-overlay-network \
    -p 8788:8788 -p 8787:8787 \
    -e PUBLIC_ORIGIN=https://pong.wardcrew.com \
    --memory 512m --cpus 1.0 \
    pong3ds:latest
'
```

Verify:

```bash
curl http://192.168.4.29:8788/healthz     # {"ok":true,...}
nc -z 192.168.4.29 8787 && echo "3DS port open"
```

## Making it public

The tunnel already has a `pong.wardcrew.com` hostname, pointed at
`http://captain-nginx`. Change that one field to:

```
http://pong-server:8788
```

in **Cloudflare Zero Trust → Networks → Tunnels → (tunnel) → Public Hostnames**.

Verified working: a container on `captain-overlay-network` resolves
`pong-server` by name, and cloudflared runs on that network. `http://172.17.0.1:8788`
(the docker bridge gateway) also works and is the pattern `business.wardcrew.com`
already uses.

### Cloudflare settings that matter

- **Cache: bypass `/api/*`.** Without this a cached `/api/rpc` response freezes
  the match for that client. This is the single most likely production-only
  failure.
- Leave **chunked encoding enabled** — SSE depends on it.
- WebSockets are on by default. If they turn out to be blocked, nothing breaks:
  the client's ladder falls to SSE and then long-poll automatically, and all
  three are verified working.

## Alternative: deploy as a CapRover app

`captain-definition` at the repo root points at `deploy/Dockerfile`, so CapRover
can build this directly. Requires dashboard access:

1. Apps → create `pong`
2. HTTP Settings → container HTTP port **8788**, add domain `pong.wardcrew.com`, enable HTTPS
3. App Configs → Port Mapping → host `8787` → container `8787` (the 3DS LAN path)
4. Deploy: connect the GitHub repo, or `caprover deploy` from a workstation

Going this route, leave the tunnel hostname pointed at `http://captain-nginx` —
CapRover's nginx then routes by hostname.

## Updating

```bash
rsync -az --delete ... ./ r@rdesktop.local:~/pong3ds/

ssh r@rdesktop.local '
  cd ~/pong3ds
  docker build -f deploy/Dockerfile -t pong3ds:latest .
  docker rm -f pong-server
  docker run -d --name pong-server --restart unless-stopped \
    --network captain-overlay-network -p 8788:8788 -p 8787:8787 \
    -e PUBLIC_ORIGIN=https://pong.wardcrew.com --memory 512m --cpus 1.0 \
    pong3ds:latest
'
```

**Recreate the container, do not `docker restart` it.** A restart reuses the
image the container was created from, so a freshly built image is silently
ignored and you debug the old binary. This bit us once; the symptom was the
integration test reporting a snapshot rate that had already been fixed.

Confirm the new build is actually live:

```bash
make -C 3ds/test -f Makefile.host play ARGS="--host 192.168.4.29 --port 8787 --seconds 9"
```

## Operational checks

```bash
curl -s http://192.168.4.29:8788/api/stats | jq
```

Reports sessions grouped by transport, room count, long-poll waiters, and tick
drift p50/p95. If tick drift p95 climbs above a few milliseconds the host is
contended and the simulation is falling behind — that box runs 200+ containers,
so it is worth watching.
