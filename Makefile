CC := cc
OPTIMIZE_FLAG ?= -s -Ofast
CFLAGS := -Wall -Wextra $(OPTIMIZE_FLAG)
LDLIBS := -lm

ifndef AUDIO_LIB
	# MINIAL is default audio lib on Windows, since no linking to external
	# libraries is required. Binary will be larger, but release will not
	# have to be packaged with additional DLL files.
	ifeq ($(OS),Windows_NT)
		AUDIO_LIB = MINIAL
	else
		AUDIO_LIB = SDL2
	endif
endif

ifeq ($(AUDIO_LIB), SDL2)
	CFLAGS += $(shell sdl2-config --cflags) -DAUDIO_DRIVER_SDL
	LDLIBS += $(shell sdl2-config --libs)

else ifeq ($(AUDIO_LIB), MINIAL)
	CFLAGS += -DAUDIO_DRIVER_MINIAL
	ifneq ($(OS), Windows_NT)
		LDLIBS += -lpthread -ldl
	endif

else ifeq ($(AUDIO_LIB),SOKOL)
	CFLAGS += -DAUDIO_DRIVER_SOKOL

	ifeq ($(OS),Windows_NT)
		LDLIBS += -lkernel32 -lole32
	else
		LDLIBS += -lasound -lpthread
	endif

else ifeq ($(AUDIO_LIB),NONE)
	CFLAGS += -DAUDIO_DRIVER_NONE

else
	# If AUDIO_LIB was set incorrectly, do not assume default AUDIO_LIB,
	# instead abort make.
	AUDIO_LIB_FAILURE = 1
endif

all: audio_lib_check minigbs
minigbs: minigbs.c audio.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS) 
minigbs.o: minigbs.c minigbs.h audio.h sokol_audio.h
audio.o: audio.c audio.h minigbs.h

audio_lib_check:
ifdef AUDIO_LIB_FAILURE
	$(error The audio library "$(AUDIO_LIB)" is not supported)
endif
clean:
	rm -f minigbs minigbs.o audio.o
help:
	@echo Options:
	@echo \ \ AUDIO_LIB=\[SDL2\|MINIAL\|SOKOL\|NONE\]
	@echo \ \ \ \ Use SDL2, MINIAL, SOKOL or NONE for output audio library.
	@echo \ \ \ \ NONE will disable audio\; useful for debugging.
	@echo \ \ \ \ MINIAL is default on Windows, other platforms use SDL2 by default.
	@echo
