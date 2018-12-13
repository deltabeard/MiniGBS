.POSIX:
.SUFFIXES:
CC = cc
CFLAGS = -std=c99 -Wall -s -Ofast
LDLIBS = -lm

ifneq ($(findstring SOKOL,$(AUDIO_DRIVER)),)
	CFLAGS += -DAUDIO_DRIVER_SOKOL
	ifeq ($(OS),Windows_NT)
		LDLIBS += -lkernel32 -lole32
	else
		LDLIBS += -lasound -lpthread
	endif
else
	CFLAGS += $(shell sdl2-config --cflags) -DAUDIO_DRIVER_SDL
	LDLIBS += $(shell sdl2-config --libs)
endif

all: minigbs
minigbs: minigbs.o audio.o
	$(CC) $(LDFLAGS) -o minigbs minigbs.o audio.o $(LDLIBS) 
minigbs.o: minigbs.c minigbs.h audio.h sokol_audio.h
audio.o: audio.c audio.h minigbs.h
clean:
	rm -f minigbs minigbs.o audio.o

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -c $<
