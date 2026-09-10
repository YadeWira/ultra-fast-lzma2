SRC = $(wildcard ../*.c) ../fuzzer/datagen.c bench.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

# Use -fPIC for obj compatibility with shared lib build
CFLAGS:=-Wall -O2 -pthread -fPIC
CC:=gcc

ASFLAGS:=

x86_64:=0
arm64:=0
EXT:=

ifeq ($(OS),Windows_NT)
	CFLAGS+=-DFL2_DLL_EXPORT=1
	EXT:=.exe
ifeq ($(PROCESSOR_ARCHITECTURE),AMD64)
	ASFLAGS+=-DMS_x64_CALL=1
	x86_64:=1
endif
else
	PROC_ARCH:=$(shell uname -p)
ifneq ($(PROC_ARCH),x86_64)
	PROC_ARCH:=$(shell uname -m)
endif
ifneq ($(arm64),1)
ifeq ($(PROC_ARCH),x86_64)
	ASFLAGS+=-DMS_x64_CALL=0
	x86_64:=1
endif
endif
ifeq ($(PROC_ARCH),aarch64)
	arm64:=1
endif
ifeq ($(PROC_ARCH),arm64)
	arm64:=1
endif
endif

ifeq ($(x86_64),1)
	CFLAGS+=-DLZMA2_DEC_OPT
	OBJ+=../lzma_dec_x86_64.o
endif

ifeq ($(arm64),1)
	CFLAGS+=-DLZMA2_DEC_OPT
	OBJ+=../lzma_dec_arm64.o
endif

bench : $(OBJ)
	$(CC) -pthread -o bench$(EXT) $(OBJ)

-include $(DEP)

%.d: %.c
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@

clean:
	rm -f bench$(EXT) $(OBJ) $(DEP)
