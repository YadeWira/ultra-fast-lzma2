OBJ=file_test.o
LIB:=-static -L../ -luf-lzma2	

CFLAGS:=-Wall -O1 -pthread
CC:=gcc

EXT:=
ifeq ($(OS),Windows_NT)
	EXT:=.exe
	LIB:=../libuf-lzma2.dll
endif
CFLAGS+=-I../

file_test : $(OBJ)
	$(CC) -pthread -o file_test$(EXT) $(OBJ) $(LIB)

clean:
	rm -f file_test$(EXT) $(OBJ)
