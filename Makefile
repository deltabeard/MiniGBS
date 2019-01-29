CC := cc
OPTIMIZE_FLAG = -s -Ofast
CFLAGS = -Wall -Wextra $(OPTIMIZE_FLAG)
LDLIBS = -lm

ifeq ($(AUDIO_LIB),SOKOL)
	CFLAGS += -DAUDIO_DRIVER_SOKOL

	ifeq ($(OS),Windows_NT)
		LDLIBS += -lkernel32 -lole32
	else
		LDLIBS += -lasound -lpthread
	endif

else ifeq ($(AUDIO_LIB),NONE)
	CFLAGS += -DAUDIO_DRIVER_NONE

else ifeq ($(AUDIO_LIB), MINIAL)
	CFLAGS += -DAUDIO_DRIVER_MINIAL
	ifneq ($(OS), Windows_NT)
		LDLIBS += -lpthread -lm -ldl
	endif

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
help:
	@echo Options:
	@echo \ \ AUDIO_LIB\=\[SOKOL\|SDL2\|NONE\]
	@echo \ \ \ \ Use SOKOL,SDL2 or NONE for output audio library.
	@echo \ \ \ \ NONE will disable audio\; useful for debugging.
	@echo \ \ \ \ SDL2 is default.
	@echo
