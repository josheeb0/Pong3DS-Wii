# Desktop build: macOS, Linux and Windows (MinGW/MSYS2).
#
# Compiles the shared interface and renderer seam with the SDL3 backend. No
# devkitPro, no console toolchain -- which is the point of the seam.
#
# SDL3:  macOS    brew install sdl3
#        Linux    libsdl3-dev, or built from source (see the CI workflow)
#        Windows  MSYS2: pacman -S mingw-w64-ucrt-x86_64-SDL3

CC      ?= cc
# Stamped by CI with the run number; 0 marks a workstation build.
BUILD_ID ?= 0

CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -g
# Appended, not folded into the ?= above: that one is overridable, and an
# override would otherwise silently strip the version out of the binary.
CFLAGS  += -DPONG_BUILD_ID=$(BUILD_ID)
ROOT    := ..
INC     := -I$(ROOT)/src -I$(ROOT)/src/gfx -I$(ROOT)/src/ui -I$(ROOT)/3ds/source/game -I$(ROOT)/3ds/source/net
# Find SDL3 without requiring PKG_CONFIG_PATH to be set by hand. Homebrew
# keeps sdl3.pc under its own opt prefix, which pkg-config does not search by
# default, and "install it and it still does not build" is a poor first
# impression.
# Passed inline rather than exported: make's `export` does not reliably reach
# $(shell) in the version macOS ships, so the path goes on the command itself.
# Windows produces an .exe and needs Winsock linked explicitly; nothing else
# about the build differs, which is the point of keeping the platform
# differences inside net_pc.c rather than here.
ifeq ($(OS),Windows_NT)
  EXE  := .exe
  PLAT_LIBS := -lws2_32
else
  EXE  :=
  PLAT_LIBS :=
endif

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

pong-pc$(EXE): $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(SDL) $(PLAT_LIBS) -lm

all: pong-pc$(EXE)

run: pong-pc$(EXE)
	./pong-pc$(EXE)

clean:
	rm -f pong-pc pong-pc.exe

.PHONY: all run clean
