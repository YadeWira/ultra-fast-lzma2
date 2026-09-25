SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

CFLAGS:=-Wall -O2 -pthread -fPIC
CC:=gcc
AR:=ar -rcs
RM:=rm -rf

ASFLAGS :=

SONAME:=libuf-lzma2.so.1
REAL_NAME:=libuf-lzma2.so.1.0
LINKER_NAME=libuf-lzma2.so
STATIC_LIBNAME=libuf-lzma2.a

x86_64:=0
arm64:=0

# runs the test programs through a wrapper, for example RUN=qemu-aarch64 after a cross build
RUN:=

ifeq ($(OS),Windows_NT)
	CFLAGS+=-DUF2_DLL_EXPORT=1
	LINKER_NAME=libuf-lzma2.dll
	SONAME:=$(LINKER_NAME)
	REAL_NAME:=$(LINKER_NAME)
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
	OBJ+=lzma_dec_x86_64.o
endif

ifeq ($(arm64),1)
	CFLAGS+=-DLZMA2_DEC_OPT
	OBJ+=lzma_dec_arm64.o
endif

libuf-lzma2 : $(OBJ)
	@echo "Build static & dynamic library."
	$(CC) -shared -pthread -Wl,-soname,$(SONAME) -o $(REAL_NAME) $(OBJ)
	$(AR) $(STATIC_LIBNAME) $(OBJ)
	@echo "Library build SUCCESS."
	
-include $(DEP)

%.d: %.c
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@

DESTDIR:=
PREFIX:=/usr/local
LIBDIR:=$(DESTDIR)$(PREFIX)/lib

.PHONY: install
install:
ifeq ($(OS),Windows_NT)
	strip -g $(REAL_NAME)
else
	mkdir -p $(LIBDIR)
	cp $(REAL_NAME) $(LIBDIR)/$(REAL_NAME)
	strip -g $(LIBDIR)/$(REAL_NAME)
	chmod 0755 $(LIBDIR)/$(REAL_NAME)
	cd $(LIBDIR) && ln -sf $(REAL_NAME) $(LINKER_NAME)
	ldconfig $(LIBDIR)
	mkdir -p $(DESTDIR)$(PREFIX)/include
	cp uf-lzma2.h $(DESTDIR)$(PREFIX)/include/
	cp uf2_errors.h $(DESTDIR)$(PREFIX)/include/
endif

.PHONY: uninstall
uninstall:
ifeq ($(OS),Windows_NT)
	rm -f libuf-lzma2.dll
else
	rm -f $(LIBDIR)/$(LINKER_NAME)
	rm -f $(LIBDIR)/$(REAL_NAME)
	ldconfig $(LIBDIR)
	rm -f $(DESTDIR)$(PREFIX)/include/uf-lzma2.h
	rm -f $(DESTDIR)$(PREFIX)/include/uf2_errors.h
endif

.PHONY: test
test:libuf-lzma2
	$(MAKE) -C ./test file_test
	$(RUN) test/file_test radix_engine.h
	@echo "File compression/decompression test completed."

# The .xz reader and writer, one-shot and streaming, against files written by
# xz and 7-Zip (test/xz) and against the library's own output; then xz itself
# tests some of that output, if it is installed.
XZ_OUT:=test/xz_out

.PHONY: check
check: test
	$(CC) $(CFLAGS) -I. -o test/crc_check test/crc_check.c $(STATIC_LIBNAME) -pthread
	$(RUN) test/crc_check
	$(CC) $(CFLAGS) -I. -o test/xz_check test/xz_check.c $(STATIC_LIBNAME) -pthread
	$(RM) $(XZ_OUT) && mkdir -p $(XZ_OUT)
	$(RUN) test/xz_check test/xz $(XZ_OUT)
	@if command -v xz >/dev/null 2>&1; then \
		for f in $(XZ_OUT)/*.xz; do xz -t "$$f" || exit 1; done; \
		echo "xz accepts every .xz file written."; \
	else \
		echo "xz is not installed: the output was not tested with xz."; \
	fi
	@echo "All checks passed."

.PHONY: clean
clean:
	$(RM) $(REAL_NAME) $(STATIC_LIBNAME) $(OBJ) $(DEP)
	$(RM) test/crc_check test/xz_check $(XZ_OUT)
	$(MAKE) -C ./test clean
