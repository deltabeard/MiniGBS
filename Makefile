.POSIX:
.SUFFIXES:
CC := cc
OPTIMIZE_FLAG = -s -Ofast
CFLAGS = -std=c99 -pedantic -Wall -Wextra $(OPTIMIZE_FLAG)
LDLIBS = -lm

ifneq ($(findstring SOKOL,$(AUDIO_DRIVER)),)
	CFLAGS += -DAUDIO_DRIVER_SOKOL
	LDLIBS += -lasound -lpthread
else
	CFLAGS += $(shell sdl2-config --cflags) -DAUDIO_DRIVER_SDL
	LDLIBS += $(shell sdl2-config --libs)
endif

all: minigbs
minigbs: minigbs.o audio.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS) 
minigbs.o: minigbs.c minigbs.h audio.h sokol_audio.h
audio.o: audio.c audio.h minigbs.h
clean:
	rm -f minigbs minigbs.o audio.o

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -c $<
