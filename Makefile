SRC		:= minigbs.c audio.c
OPT		:= -s -Ofast
CFLAGS	:= $(shell sdl2-config --cflags) -std=gnu99 -Wall
LDFLAGS	:= $(shell sdl2-config --libs) -lm

minigbs: $(SRC) minigbs.h
	$(CC) $(CFLAGS) $(OPT) $(SRC) -o $@ $(LDFLAGS)

clean:
	$(RM) minigbs
