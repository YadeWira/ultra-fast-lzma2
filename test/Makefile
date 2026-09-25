OBJ=file_test.o
# the static library, named directly so that the shared one next to it is not picked
LIB:=../libuf-lzma2.a

CFLAGS:=-Wall -O1 -pthread
CC:=gcc

EXT:=
ifeq ($(OS),Windows_NT)
	EXT:=.exe
	LIB:=../libuf-lzma2.dll
endif
# override: CFLAGS given on the command line must not drop the include path
override CFLAGS+=-I../

file_test : $(OBJ)
	$(CC) $(CFLAGS) -pthread -o file_test$(EXT) $(OBJ) $(LIB)

clean:
	rm -f file_test$(EXT) $(OBJ)
