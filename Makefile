SRC     := minigbs.c audio.c
CFLAGS  := -g -O0 -D_GNU_SOURCE -std=gnu99
LDFLAGS := -lSDL2 -lm -ltinfo

minigbs: $(SRC) minigbs.h
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

clean:
	$(RM) minigbs
