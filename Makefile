PS2DEV ?= $(HOME)/ps2dev
GSDEV ?= $(PS2DEV)/gsKit

EE_BIN = main.elf
EE_OBJS = main.o

EE_INCS += -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I$(GSDEV)/include
EE_LDFLAGS += -L$(PS2SDK)/ee/lib -L$(GSDEV)/lib
EE_LIBS += -lmc -lgskit_toolkit -lgskit -ldmakit -laudsrv -lpad -lkernel -lc -lpacket -ldma

all: $(EE_BIN)

clean:
	rm -f $(EE_OBJS) $(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
