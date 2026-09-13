# Pong3DS — cross-play Pong between a browser and a Nintendo 3DS.
#
#   make dev        run server + web client locally
#   make 3ds        build the .3dsx
#   make cia        build the installable .cia
#   make send       push the .3dsx to a 3DS over wifi (no SD card)
#   make test       run every test suite
#   make verify     full gate: codegen drift, tests, 3DS build

SHELL := /bin/bash
export DEVKITPRO ?= /opt/devkitpro
export DEVKITARM ?= $(DEVKITPRO)/devkitARM

# Set once: export N3DS_IP=192.168.4.x  (Homebrew Launcher shows it)
N3DS_IP ?=

.PHONY: all proto server web dev 3ds cia send tools mbedtls test test-sim test-proto verify clean distclean

all: proto server web 3ds

## ---------------------------------------------------------------- codegen --
proto:
	node tools/gen-protocol.mjs
	# The simulation trace is regenerated with the protocol, because both are
	# derived artefacts of the TypeScript and a stale trace would let the C
	# simulation drift from the server's rules unnoticed.
	npx tsx tools/gen-sim-trace.ts

## ----------------------------------------------------------------- server --
server:
	cd server && npm install --silent && npm run build

dev:
	@echo "server on :8788 (http+ws) and :8787 (tcp), web on :5173"
	@trap 'kill 0' EXIT; \
	  (cd server && npx tsx watch src/index.ts) & \
	  (cd web && npm run dev) & \
	  wait

## -------------------------------------------------------------------- web --
web:
	cd web && npm install --silent && npm run build

## -------------------------------------------------------------------- 3ds --
mbedtls:
	./3ds/vendor/build-mbedtls.sh

tools:
	./3ds/vendor/fetch-tools.sh

3ds: mbedtls proto
	$(MAKE) -C 3ds

cia: 3ds tools
	./3ds/build-cia.sh 3ds/pong3ds.elf 3ds/pong3ds.cia

# Push straight into the Homebrew Launcher over wifi. This is the development
# loop -- roughly a second, and the SD card never leaves the console.
send: 3ds
ifeq ($(strip $(N3DS_IP)),)
	@echo "Set N3DS_IP first:  make send N3DS_IP=192.168.4.x"
	@echo "(the Homebrew Launcher shows the address while it waits for a netload)"
	@exit 1
else
	$(DEVKITPRO)/tools/bin/3dslink -a $(N3DS_IP) 3ds/pong3ds.3dsx
endif

## ------------------------------------------------------------------ tests --
test: test-proto test-sim

test-sim:
	cd server && npx vitest run

# Compiles the 3DS codec with the HOST compiler and checks it against byte
# vectors the TypeScript encoder produced. Catches a C/TS disagreement on a
# laptop instead of on hardware.
test-proto: proto
	$(MAKE) -C 3ds/test -f Makefile.host run

verify: proto
	@echo "==> generated files must be up to date"
	@git diff --exit-code -- shared/gen 3ds/source/net/pong_proto.h \
	    3ds/source/net/pong_proto.c 3ds/test/golden_data.h \
	    3ds/source/game/pong_trig.h \
	  || (echo "generated files are stale -- commit the result of 'make proto'"; exit 1)
	$(MAKE) test
	$(MAKE) 3ds
	@echo ""
	@echo "verify: OK"

## ------------------------------------------------------------------ clean --
clean:
	$(MAKE) -C 3ds clean 2>/dev/null || true
	$(MAKE) -C 3ds/test -f Makefile.host clean 2>/dev/null || true
	rm -rf server/dist web/dist 3ds/.ciabuild

distclean: clean
	rm -rf server/node_modules web/node_modules 3ds/vendor/mbedtls 3ds/vendor/.src
