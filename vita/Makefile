# PS Vita build. Needs VitaSDK (https://vitasdk.org) on PATH.
#
# NOT YET COMPILED. VitaSDK was not installed on the machine this was written
# on, so this file and src/gfx/gfx_vita.c are careful reading of the toolchain's
# conventions rather than anything that has run. The first person with a VitaSDK
# should expect to fix something here; the renderer seam is what makes that a
# contained job rather than a port.

TARGET   := pong-vita
TITLE    := PONG3DSV1
ROOT     := ..

CC       := arm-vita-eabi-gcc
CFLAGS   := -std=c99 -Wall -Wextra -O2 -fno-lto \
            -I$(ROOT)/src/gfx -I$(ROOT)/src/ui \
            -I$(ROOT)/3ds/source/game -I$(ROOT)/3ds/source/net
LIBS     := -lvita2d -lSceDisplay_stub -lSceGxm_stub -lSceSysmodule_stub \
            -lSceCtrl_stub -lSceTouch_stub -lSceCommonDialog_stub \
            -lSceNet_stub -lSceNetCtl_stub -lm

SRC := main_vita.c \
       $(ROOT)/src/gfx/vita/gfx_vita.c \
       $(ROOT)/src/ui/render.c \
       $(ROOT)/3ds/source/net/pong_proto.c \
       $(ROOT)/3ds/source/game/client.c

all: $(TARGET).vpk

$(TARGET).elf: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

$(TARGET).velf: $(TARGET).elf
	arm-vita-eabi-strip -g $<
	vita-elf-create $< $@

eboot.bin: $(TARGET).velf
	vita-make-fself -s $< $@

param.sfo:
	vita-mksfoex -s TITLE_ID=$(TITLE) "Pong Multiplayer" $@

$(TARGET).vpk: eboot.bin param.sfo
	vita-pack-vpk -s param.sfo -b eboot.bin $@

clean:
	rm -f $(TARGET).elf $(TARGET).velf eboot.bin param.sfo $(TARGET).vpk

.PHONY: all clean
