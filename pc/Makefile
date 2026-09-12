# PC build. Needs SDL3 (brew install sdl3 / apt install libsdl3-dev).
#
# Compiles the shared interface and renderer seam with the SDL3 backend. No
# devkitPro, no console toolchain -- which is the point of the seam.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -g
ROOT    := ..
INC     := -I$(ROOT)/src/gfx -I$(ROOT)/src/ui -I$(ROOT)/3ds/source/game -I$(ROOT)/3ds/source/net
# Find SDL3 without requiring PKG_CONFIG_PATH to be set by hand. Homebrew
# keeps sdl3.pc under its own opt prefix, which pkg-config does not search by
# default, and "install it and it still does not build" is a poor first
# impression.
# Passed inline rather than exported: make's `export` does not reliably reach
# $(shell) in the version macOS ships, so the path goes on the command itself.
BREW_SDL3 := $(shell brew --prefix sdl3 2>/dev/null)
PKGPATH   := $(if $(BREW_SDL3),$(BREW_SDL3)/lib/pkgconfig:)$(PKG_CONFIG_PATH)
SDL       := $(shell PKG_CONFIG_PATH="$(PKGPATH)" pkg-config --cflags --libs sdl3 2>/dev/null)

ifeq ($(strip $(SDL)),)
$(error SDL3 not found. macOS: brew install sdl3 - Debian/Ubuntu: apt install libsdl3-dev)
endif

SRC := main_sdl.c net_pc.c \
       $(ROOT)/src/gfx/sdl3/gfx_sdl3.c \
       $(ROOT)/src/ui/desktop.c \
       $(ROOT)/3ds/source/net/pong_proto.c \
       $(ROOT)/3ds/source/game/client.c

pong-pc: $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(SDL) -lm

run: pong-pc
	./pong-pc

clean:
	rm -f pong-pc

.PHONY: run clean
