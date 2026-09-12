# Deploying Pong3DS

The server is one container exposing two ports:

| Port | Protocol | Who uses it |
|------|----------|-------------|
| 8788 | HTTP + WebSocket + SSE | the browser client, and the 3DS when it is away from home |
| 8787 | raw TCP | the 3DS on the LAN (fast path, full 60Hz) |

8787 cannot go through nginx or the Cloudflare tunnel — it is not HTTP. It is
published on the host so the 3DS can reach `192.168.4.29:8787` directly.

---

## Current deployment (what is running now)

Built and run directly with Docker on `rdesktop.local`, joined to
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
