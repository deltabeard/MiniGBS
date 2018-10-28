#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct GBSHeader;
struct Config;

extern void process_cpu(void);

void audio_init        (void);
void audio_update      (void);
void audio_reset       (void);
void audio_write       (uint16_t addr, uint8_t val);
void audio_update_rate (void);
void audio_get_notes   (uint16_t[static 4]);
void audio_get_vol     (uint8_t vol[static 8]);

struct GBSHeader {
	char     id[3];
	uint8_t  version;
	uint8_t  song_count;
	uint8_t  start_song;
	uint16_t load_addr;
	uint16_t init_addr;
	uint16_t play_addr;
	uint16_t sp;
	uint8_t  tma;
	uint8_t  tac;
	char     title[32];
	char     author[32];
	char     copyright[32];
} __attribute__((packed));

struct {
	union {
		uint16_t af;
		struct {
			union {
				struct { uint8_t _pad:4, c:1, h:1, n:1, z:1; };
				uint8_t all;
			} flags;
			uint8_t a;
		};
	};
	union { uint16_t bc; struct { uint8_t c, b; }; };
	union { uint16_t de; struct { uint8_t e, d; }; };
	union { uint16_t hl; struct { uint8_t l, h; }; };
	uint16_t sp, pc;
} regs;

struct Config {
	int song_no;
};

extern uint8_t* mem;

#define MAX(a, b) ({ typeof(a) _a = (a); typeof(b) _b = (b); _a >  _b ? _a : _b; })
#define MIN(a, b) ({ typeof(a) _a = (a); typeof(b) _b = (b); _a <= _b ? _a : _b; })
#define countof(x) (sizeof(x)/sizeof(*x))
