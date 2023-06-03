CC := cc
OPT := -s -O2

# Required build flags.
CFLAGS += -Wall -Wextra $(OPT)
LDLIBS += -lm

ifndef AUDIO_LIB
	# MINIAUDIO is default audio lib on Windows, since no linking to
	# external libraries is required. Binary will be larger, but release
	# will not have to be packaged with additional DLL files.
	ifeq ($(OS),Windows_NT)
		AUDIO_LIB := MINIAUDIO
	else
		AUDIO_LIB := SDL2
	endif
endif

ifeq ($(AUDIO_LIB), SDL2)
	CFLAGS += $(shell sdl2-config --cflags) -DAUDIO_DRIVER_SDL
	LDLIBS += $(shell sdl2-config --libs)

else ifeq ($(AUDIO_LIB), MINIAUDIO)
	CFLAGS += -DAUDIO_DRIVER_MINIAUDIO
	ifneq ($(OS), Windows_NT)
		LDLIBS += -lpthread -ldl
	endif

else ifeq ($(AUDIO_LIB),NONE)
	CFLAGS += -DAUDIO_DRIVER_NONE

else
	# If AUDIO_LIB was set incorrectly, do not assume default AUDIO_LIB,
	# instead abort make.
	AUDIO_LIB_FAILURE = 1
endif

all: audio_lib_check minigbs
minigbs: minigbs.o minigb_apu.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS) 
minigbs.o: minigbs.c minigb_apu.h
minigb_apu.o: minigb_apu.c minigb_apu.h

audio_lib_check:
ifdef AUDIO_LIB_FAILURE
	$(error The audio library "$(AUDIO_LIB)" is not supported)
endif
clean:
	rm -f minigbs minigbs.o minigb_apu.o
help:
	@echo Options:
	@echo \ \ AUDIO_LIB=\[SDL2\|MINIAUDIO\|NONE\]
	@echo \ \ \ \ Use SDL2, MINIAUDIO, or NONE for output audio library.
	@echo \ \ \ \ NONE will disable audio\; useful for debugging.
	@echo \ \ \ \ MINIAUDIO is default on Windows, other platforms use SDL2 by default.
	@echo
