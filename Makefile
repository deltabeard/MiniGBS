.POSIX:
.SUFFIXES:
CC := cc
OPTIMIZE_FLAG = -s -Ofast
CFLAGS = -Wall -Wextra $(OPTIMIZE_FLAG)
LDLIBS = -lm

# Default AUDIO_LIB is SDL2
ifndef AUDIO_LIB
	AUDIO_LIB = SDL2
endif

ifeq ($(AUDIO_LIB),SOKOL)
	CFLAGS += -DAUDIO_DRIVER_SOKOL

	ifeq ($(OS),Windows_NT)
		LDLIBS += -lkernel32 -lole32
	else
		LDLIBS += -lasound -lpthread
	endif
endif

ifeq ($(AUDIO_LIB),NONE)
	CFLAGS += -DAUDIO_DRIVER_NONE
endif

ifeq ($(AUDIO_LIB),SDL2)
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
help:
	@echo Options:
	@echo \ AUDIO_LIB\=\[SOKOL\|SDL2\]\	\
		Use SOKOL or SDL2 for output audio library. SDL2 is default.
	@echo

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -c $<
