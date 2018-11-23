.POSIX:
.SUFFIXES:
CC = cc
CFLAGS = -std=gnu99 -W -s -Ofast
LDLIBS = -lm -lasound -lpthread

all: minigbs
minigbs: minigbs.o audio.o
	$(CC) $(LDFLAGS) -o minigbs minigbs.o audio.o $(LDLIBS) 
minigbs.o: minigbs.c minigbs.h audio.h sokol_audio.h
	$(CC) -c $(CFLAGS) minigbs.c
audio.o: audio.c audio.h minigbs.h
	$(CC) -c $(CFLAGS) audio.c
clean:
	rm -f minigbs minigbs.o audio.o

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -c $<
