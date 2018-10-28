SRC     := minigbs.c audio.c
OPT     := -s -Ofast
CFLAGS  := -std=gnu99 -Wall
LDFLAGS := -lSDL2 -lm

minigbs: $(SRC) minigbs.h
	$(CC) $(CFLAGS) $(OPT) $(SRC) -o $@ $(LDFLAGS)

clean:
	$(RM) minigbs
