Presented is a stripped down version of MiniGBS. The final objective is to
create a Game Boy Audio Processing Unit (APU) within a single-header C99
library.

The `minigbs` player supports optional playlist files. Use `-p <file>` to
specify a playlist when running the program:

```
minigbs music.gbs -p playlist.txt
```

Playlist format:

* Lines starting with `#` are ignored.
* `T<track>,<seconds>` plays the specified track for the given duration.
* `J<line>,<count>` jumps to another line in the playlist. A count of `-1`
  means the jump is executed indefinitely.

Press `n` to skip to the next entry or `q`/`Ctrl+C` to quit during playback.
