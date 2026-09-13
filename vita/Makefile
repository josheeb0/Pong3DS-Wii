# PS Vita build. Needs VitaSDK (https://vitasdk.org) on PATH.
#
# COMPILES, not yet run on hardware. Written without a VitaSDK to hand and first
# built in CI, where every source file compiled and only the link failed -- for
# a missing stub library, since corrected. Whether it actually draws anything on
# a Vita is still unknown.

TARGET   := pong-vita
TITLE    := PONG3DSV1
ROOT     := ..

CC       := arm-vita-eabi-gcc
# -Wl,-q is not optional on this platform.
#
# It keeps the relocation entries in the ELF, and vita-elf-create REQUIRES them
# to produce a loadable module. Without it everything still compiles, links and
# packages into a .vpk that installs -- and then loads unrelocated, so every
# absolute address is wrong and the first write to a global data-aborts.
#
# That is what three hardware crashes were: _init_vita_heap faulting on a store
# to its own globals before main() ran. vita2d's own Makefile passes this flag
# on every line; mine did not, and nothing along the way said so.
LDFLAGS  := -Wl,-q

# Stamped by CI with the run number; 0 marks a workstation build.
BUILD_ID ?= 0

CFLAGS   := -std=c99 -Wall -Wextra -O2 -fno-lto \
            -DPONG_BUILD_ID=$(BUILD_ID) \
            -I$(ROOT)/src -I$(ROOT)/src/gfx -I$(ROOT)/src/ui -I$(ROOT)/pc \
            -I$(ROOT)/3ds/source/game -I$(ROOT)/3ds/source/net
# No SceSharedFb here. The vitasdk image's prebuilt libvita2d.a wants it and
# the image provides no such stub -- the two disagree. CI builds vita2d from
# source against the stubs that are actually installed, which removes the
# reference rather than trying to satisfy it.
# SceLibKernel and friends are what newlib's syscall layer actually calls on
# this platform -- malloc, stdio, file I/O all route through them. Leaving them
# out still LINKS, because the references resolve weakly, and then the first
# stdio call jumps through an uninitialised pointer at runtime.
#
# That is precisely what the first two hardware crashes were: identical stack
# pointer and caller, a different garbage PC each run, and the first one landing
# inside newlib's own reent_list. A missing library, not a missing check.
#
# No SceThreadmgr here: this SDK does not ship that stub, and thread management
# comes through SceLibKernel. Checked by the linker rather than assumed, after
# guessing a stub name wrong twice already.
LIBS     := -lvita2d \
            -lSceDisplay_stub -lSceGxm_stub -lSceSysmodule_stub \
            -lSceCtrl_stub -lSceTouch_stub -lSceCommonDialog_stub \
            -lSceAppMgr_stub \
            -lSceNet_stub -lSceNetCtl_stub \
            -lSceLibKernel_stub -lSceIofilemgr_stub -lSceSysmem_stub \
            -lSceProcessmgr_stub -lSceRtc_stub \
            -lm -lc

# The DESKTOP interface, not the handheld one: the Vita has a single 960x544
# screen and emulating the 3DS's two panels would waste most of it.
SRC := main_vita.c net_vita.c ime_vita.c \
       $(ROOT)/src/gfx/vita/gfx_vita.c \
       $(ROOT)/src/ui/desktop.c \
       $(ROOT)/3ds/source/net/pong_proto.c \
       $(ROOT)/3ds/source/game/client.c

all: $(TARGET).vpk hello.vpk

# A minimal vita2d app sharing this Makefile's toolchain and libraries, so a
# failure here is the platform and a failure only in the game is the game.
hello.elf: hello.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

hello.velf: hello.elf
	arm-vita-eabi-strip -g $<
	vita-elf-create $< $@

hello-eboot.bin: hello.velf
	vita-make-fself -s $< $@

hello-param.sfo:
	vita-mksfoex -s TITLE_ID=PONGHELLO "Pong Vita Hello" $@

hello.vpk: hello-eboot.bin hello-param.sfo
	vita-pack-vpk -s hello-param.sfo -b hello-eboot.bin $@

$(TARGET).elf: $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LIBS)

# The symbols are copied aside BEFORE stripping. vita-elf-create needs a
# stripped input, and strip works in place -- so without this the only build
# that exists is the one that cannot be read, which is exactly the state the
# first crash dump arrived in.
$(TARGET).velf: $(TARGET).elf
	cp $< $(TARGET)-debug.elf
	arm-vita-eabi-strip -g $<
	vita-elf-create $< $@

eboot.bin: $(TARGET).velf
	vita-make-fself -s $< $@

param.sfo:
	vita-mksfoex -s TITLE_ID=$(TITLE) "Pong Multiplayer" $@

$(TARGET).vpk: eboot.bin param.sfo
	vita-pack-vpk -s param.sfo -b eboot.bin $@

clean:
	rm -f $(TARGET).elf $(TARGET)-debug.elf $(TARGET).velf eboot.bin param.sfo $(TARGET).vpk \
	      hello.elf hello.velf hello-eboot.bin hello-param.sfo hello.vpk

.PHONY: all clean
