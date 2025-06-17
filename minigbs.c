#include "minigb_apu.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <fcntl.h>
#endif

#ifdef AUDIO_DRIVER_SDL
#include <SDL2/SDL.h>
#endif

#ifdef AUDIO_DRIVER_MINIAUDIO
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#include "miniaudio.h"
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Some of the bitfield / casting used in here assumes little endian :("
#endif

#define ROM_BANK_SIZE	0x4000
#define ROM_BANK1_ADDR	0x4000
#define VRAM_ADDR	0x8000
#define RAM_START_ADDR	0xA000
#define RAM_STOP_ADDR	0xDFFF
#define HRAM_START_ADDR	0xFF80
#define HRAM_STOP_ADDR	0xFFFE

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
} __attribute__((packed)) GBSHeader;

struct {
	union {
		uint16_t af;
		struct {
			union {
				struct {
					uint8_t _pad : 4, c : 1, h : 1, n : 1,
						z : 1;
				};
				uint8_t all;
			} flags;
			uint8_t a;
		};
	};
	union {
		uint16_t bc;
		struct {
			uint8_t c, b;
		};
	};
	union {
		uint16_t de;
		struct {
			uint8_t e, d;
		};
	};
	union {
		uint16_t hl;
		struct {
			uint8_t l, h;
		};
	};
	uint16_t sp, pc;
} regs;

#define MAX(a, b) ({ a > b ? a : b; })

uint8_t *mem;
uint8_t *hram;

static struct GBSHeader h;
static uint8_t *	banks[32];
static uint8_t *	selected_rom_bank;
struct minigb_apu_ctx ctx;

enum playlist_type { PL_TRACK, PL_JUMP };

struct playlist_entry {
        enum playlist_type type;
        size_t line_no;
        union {
                struct {
                        uint16_t track;
                        uint32_t duration;
                } track;
                struct {
                        uint16_t line;
                        int16_t  count;
                        int16_t  executed;
                } jump;
        } data;
};

static struct playlist_entry *playlist;
static size_t                playlist_len;

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t next_flag;

static void sigint_handler(int sig)
{
        (void)sig;
        stop_flag = 1;
}

static void bank_switch(const uint8_t which)
{
	// allowing bank switch to 0 seems to break some games
	if (which > 0 && which < 32 && banks[which])
		selected_rom_bank = banks[which];
}

static void mem_write(const uint16_t addr, const uint8_t val)
{
	/* Call audio_write when writing to audio registers. */
	if (addr >= 0xFF06 && addr <= 0xFF3F)
		audio_write(&ctx, addr, val);
	/* Switch ROM banks. */
	else if (addr >= 0x2000 && addr < ROM_BANK1_ADDR)
		bank_switch(val);
	else if (addr >= RAM_START_ADDR && addr <= RAM_STOP_ADDR)
		mem[addr - RAM_START_ADDR] = val;
	else if (addr >= HRAM_START_ADDR && addr <= HRAM_STOP_ADDR)
		hram[addr - HRAM_START_ADDR] = val;

	return;
}

static uint8_t mem_read(const uint16_t addr)
{
	/* Read from ROM Bank 0. */
	if (addr < 0x4000)
		return banks[0][addr];
	/* Read from selected ROM Bank 1. */
	else if (addr >= 0x4000 && addr <= 0x7FFF)
		return selected_rom_bank[addr - 0x4000];
	else if (addr >= RAM_START_ADDR && addr <= RAM_STOP_ADDR)
		return mem[addr - RAM_START_ADDR];
	/* Read Audio registers. */
	else if (addr >= 0xFF06 && addr <= 0xFF3F)
		return audio_read(&ctx, addr);
	else if (addr >= HRAM_START_ADDR && addr <= HRAM_STOP_ADDR)
		return hram[addr - HRAM_START_ADDR];

	/* Catch-all for everything else. */
	return 0xFF;
}

static void cpu_step(void)
{
	uint8_t		op;
	uint_least16_t	x;
	uint_least16_t	y;
	uint_least16_t	z;

	if (regs.pc >= ROM_BANK1_ADDR && regs.pc < VRAM_ADDR)
		op = selected_rom_bank[regs.pc - ROM_BANK1_ADDR];
	else
		op = mem_read(regs.pc);

	x = op >> 6;
	y = (op >> 3) & 7;
	z = op & 7;

#define OP(x) &&op_##x
#define ALUY (void *)(1)

	static const void *xmap[4] = { [1] = OP(mov8), [2] = ALUY };

	static const void *zmap[4][8] = {
		[0] = { [2] = OP(ldsta16),
			[3] = OP(incdec16),
			[4] = OP(inc8),
			[5] = OP(dec8),
			[6] = OP(ld8) },
		[3] = { [6] = ALUY, [7] = OP(rst) },
	};

	static const void* ymap[4][8][8] = {
		[0] = {
			[0] = { OP(nop) , OP(stsp) , OP(stop), OP(jr)   , OP(jrcc), OP(jrcc) , OP(jrcc), OP(jrcc)  },
			[1] = { OP(ld16), OP(addhl), OP(ld16), OP(addhl), OP(ld16), OP(addhl), OP(ld16), OP(addhl) },
			[7] = { OP(rlca), OP(rrca) , OP(rla) , OP(rra)  , OP(daa) , OP(cpl)  , OP(scf) , OP(ccf)   },
		},
		[3] = {
			[0] = { OP(retcc) , OP(retcc) , OP(retcc) , OP(retcc) , OP(sth)  , OP(addsp), OP(ldh)  , OP(ldsp)  },
			[1] = { OP(pop)   , OP(ret)   , OP(pop)   , OP(reti)  , OP(pop)  , OP(jphl) , OP(pop)  , OP(sphl)  },
			[2] = { OP(jpcc)  , OP(jpcc)  , OP(jpcc)  , OP(jpcc)  , OP(stha) , OP(st16) , OP(ldha) , OP(lda16) },
			[3] = { OP(jp)    , OP(cb)    , OP(undef) , OP(undef) , OP(undef), OP(undef), OP(di)   , OP(ei)    },
			[4] = { OP(callcc), OP(callcc), OP(callcc), OP(callcc), OP(undef), OP(undef), OP(undef), OP(undef) },
			[5] = { OP(push)  , OP(call)  , OP(push)  , OP(undef) , OP(push) , OP(undef), OP(push) , OP(undef) },
		}
	};

	static const void *alu[8] = { OP(add), OP(adc), OP(sub), OP(sbc),
				      OP(and), OP(xor), OP(or),  OP(cp) };

	static const struct {
		uint8_t shift;
		uint8_t want;
	} cc[] = {
		{ 7, 0 }, // NZ
		{ 7, 1 }, // Z
		{ 4, 0 }, // NC
		{ 4, 1 }, // C
	};

	// TODO: clean this mess up
	uint8_t *	r[]   = { &regs.b, &regs.c, &regs.d, &regs.e, &regs.h,
				  &regs.l, mem - RAM_START_ADDR + regs.hl,
			   	  &regs.a };
	static uint16_t *rr[]  = { &regs.bc, &regs.de, &regs.hl, &regs.hl };
	static void *    rot[] = { &&op_rlc, &&op_rrc, &&op_rl,   &&op_rr,
				   &&op_sla, &&op_sra, &&op_swap, &&op_srl };
	static uint16_t *rp2[] = { &regs.bc, &regs.de, &regs.hl, &regs.af };

	uint8_t alu_val = 0;

#define R_READ(i)                              \
	({                                     \
		uint8_t v;                     \
		if (i == 6) {                  \
			v = mem_read(regs.hl); \
		} else {                       \
			v = *r[i];             \
		}                              \
		v;                             \
	})
#define R_WRITE(i, v)                          \
	({                                     \
		if (i == 6) {                  \
			mem_write(regs.hl, v); \
		} else {                       \
			*r[i] = v;             \
		};                             \
		*r[i];                         \
	})

	if (xmap[x] > ALUY) {
		goto *xmap[x];
	} else if (xmap[x] == ALUY) {
		alu_val = R_READ(z);
		goto *alu[y];
	} else if (zmap[x][z] > ALUY) {
		goto *zmap[x][z];
	} else if (zmap[x][z] == ALUY) {
		alu_val = mem_read(++regs.pc);
		goto *alu[y];
	} else {
		goto *ymap[x][z][y];
	}

#undef ALUY
#undef OP

#define OP(name, len, code)     \
	op_##name:              \
	{                       \
		code;           \
		regs.pc += len; \
		goto end;       \
	}
#define CHECKCC(n) (((regs.flags.all >> cc[n].shift) & 1) == cc[n].want)

#define SS(p) (((uint16_t *)&regs.bc)[p])
#define DD(p) (((uint16_t *)&regs.bc) + (p))
#define NN ((((uint16_t)mem_read(regs.pc + 2)) << 8) | mem_read(regs.pc + 1))

	OP(mov8, 1, {
		if (z == 6 && y == 6) {
			puts("HALT?");
		} else {
			R_WRITE(y, R_READ(z));
		}
	});

	OP(ldsta16, 1, {
		size_t p = y >> 1;

		if (y & 1) {
			regs.a = mem_read(*rr[p]);
		} else {
			mem_write(*rr[p], regs.a);
		}

		if (p == 2)
			regs.hl++;
		else if (p == 3)
			regs.hl--;
	});

	OP(incdec16, 1, {
		if (y & 1) {
			--*DD(y >> 1);
		} else {
			++*DD(y >> 1);
		}
	});

	OP(inc8, 1, {
		regs.flags.h = (R_READ(y) & 0xF) == 9;
		R_WRITE(y, R_READ(y) + 1);
		regs.flags.z = !R_READ(y);
		regs.flags.n = 0;
	});

	OP(dec8, 1, {
		regs.flags.h = (R_READ(y) & 0xF) == 0;
		R_WRITE(y, R_READ(y) - 1);
		regs.flags.z = !R_READ(y);
		regs.flags.n = 1;
	});

	OP(ld8, 2, {
		R_WRITE(y, mem_read(regs.pc + 1));
	});

	OP(nop, 1,
	   {
		   // skip
	   });

	OP(stsp, 3, {
		mem_write(NN + 1, regs.sp >> 8);
		mem_write(NN, regs.sp & 0xFF);
	});

	OP(stop, 2,
	   {
		   // skip
	   });

	OP(jr, 2, { regs.pc += (int8_t)mem_read(regs.pc + 1); });

	OP(jrcc, 2, {
		if (CHECKCC(y - 4)) {
			regs.pc += (int8_t)mem_read(regs.pc + 1);
		}
	});

	OP(ld16, 3, { *DD(y >> 1) = NN; });

	OP(addhl, 1, {
		uint16_t ss  = SS(y >> 1);
		regs.flags.h = (((ss & 0x0FFF) + (regs.hl & 0x0FFF)) &
				0x1000) == 0x1000;
		regs.flags.c = __builtin_add_overflow(regs.hl, ss, &regs.hl);
		regs.flags.n = 0;
	});

	OP(rlca, 1, {
		regs.flags.c = regs.a >> 7;
		regs.a       = (regs.a << 1) | regs.flags.c;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(rrca, 1, {
		regs.flags.c = regs.a & 1;
		regs.a       = (regs.a >> 1) | regs.flags.c << 7;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(rla, 1, {
		size_t newc  = regs.a >> 7;
		regs.a       = (regs.a << 1) | regs.flags.c;
		regs.flags.c = newc;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(rra, 1, {
		size_t newc  = regs.a & 1;
		regs.a       = (regs.a >> 1) | regs.flags.c << 7;
		regs.flags.c = newc;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(daa, 1, {
		size_t up   = regs.a >> 4;
		size_t dn   = regs.a & 0xF;
		size_t newc = 0;

		if (dn >= 10 || regs.flags.h) {
			if (regs.flags.n) {
				newc |= __builtin_sub_overflow(regs.a, 0x06,
							       &regs.a);
			} else {
				newc |= __builtin_add_overflow(regs.a, 0x06,
							       &regs.a);
			}
		}

		if (up >= 10 || regs.flags.c) {
			if (regs.flags.n) {
				newc |= __builtin_sub_overflow(regs.a, 0x60,
							       &regs.a);
			} else {
				newc |= __builtin_add_overflow(regs.a, 0x60,
							       &regs.a);
			}
		}

		regs.flags.c = newc;
		regs.flags.h = 0;
		regs.flags.z = !regs.a;
	});

	OP(cpl, 1, {
		regs.a       = ~regs.a;
		regs.flags.h = 1;
		regs.flags.n = 1;
	});

	OP(scf, 1, {
		regs.flags.c = 1;
		regs.flags.h = 0;
		regs.flags.n = 0;
	});

	OP(ccf, 1, {
		regs.flags.c = !regs.flags.c;
		regs.flags.h = 0;
		regs.flags.n = 0;
	});

	OP(retcc, 1, {
		if (CHECKCC(y)) {
			regs.pc = ((mem_read(regs.sp + 1) << 8) |
				   mem_read(regs.sp)) -
				  1;
			regs.sp += 2;
		}
	});

	OP(sth, 2, { mem_write(0xFF00 + mem_read(regs.pc + 1), regs.a); });

	OP(addsp, 2, {
		regs.flags.h =
			(((regs.sp & 0x0FFF) + (mem_read(regs.pc + 1) & 0x0F)) &
			 0x1000) == 0x1000;
		regs.flags.c = __builtin_add_overflow(
			regs.sp, (int8_t)mem_read(regs.pc + 1),
			(int16_t *)&regs.sp);
		regs.flags.z = regs.flags.n = 0;
	});

	OP(ldh, 2, { regs.a = mem_read(0xFF00 + mem_read(regs.pc + 1)); });

	OP(ldsp, 2, {
		regs.hl      = regs.sp + mem_read(regs.pc + 1);
		regs.flags.h = regs.flags.n = regs.flags.z = regs.flags.c =
			0; // XXX: probably wrong
	});

	OP(pop, 1, {
		*rp2[y >> 1] = (mem_read(regs.sp + 1) << 8) | mem_read(regs.sp);
		regs.sp += 2;
	});

	OP(ret, 0, {
		regs.pc = (mem_read(regs.sp + 1) << 8 | mem_read(regs.sp));
		regs.sp += 2;
	});

	OP(reti, 0, {
		regs.pc = mem_read(regs.sp + 1) << 8 | mem_read(regs.sp);
		regs.sp += 2;
		// XXX: interrupts not implemented
	});

	OP(jphl, 0, { regs.pc = regs.hl; });

	OP(sphl, 1, { regs.sp = regs.hl; });

	OP(jpcc, 3, {
		if (CHECKCC(y)) {
			regs.pc = NN - 3;
		}
	});

	OP(stha, 1, { mem_write(0xFF00 + regs.c, regs.a); });

	OP(st16, 3, { mem_write(NN, regs.a); });

	OP(ldha, 1, { regs.a = mem_read(0xFF00 + regs.c); });

	OP(lda16, 3, { regs.a = mem_read(NN); });

	OP(jp, 0, { regs.pc = NN; });

	OP(cb, 0, {
		op = mem_read(++regs.pc);
		x  = (op >> 6);
		y  = (op >> 3) & 7;
		z  = op & 7;

		++regs.pc;

		if (x == 0) {
			goto *rot[y];
		} else if (x == 1) { // BIT
			regs.flags.z = !(R_READ(z) & (1 << y));
			regs.flags.n = 0;
			regs.flags.h = 1;
		} else if (x == 2) { // RES
			R_WRITE(z, R_READ(z) & ~(1 << y));
		} else { // SET
			R_WRITE(z, R_READ(z) | (1 << y));
		}
	});

	OP(undef, 1,
	   {
		   // skip
	   });

	OP(di, 1,
	   {
		   // XXX: interrupts not implemented
	   });

	OP(ei, 1,
	   {
		   // XXX: interrupts not implemented
	   });

	OP(callcc, 3, {
		if (CHECKCC(y)) {
			mem_write(regs.sp - 1, (regs.pc + 3) >> 8);
			mem_write(regs.sp - 2, (regs.pc + 3) & 0xFF);
			regs.sp -= 2;
			regs.pc = NN - 3;
		}
	});

	OP(push, 1, {
		mem_write(regs.sp - 2, *rp2[y >> 1] & 0xFF);
		mem_write(regs.sp - 1, *rp2[y >> 1] >> 8);
		regs.sp -= 2;
	});

	OP(call, 0, {
		mem_write(regs.sp - 1, (regs.pc + 3) >> 8);
		mem_write(regs.sp - 2, (regs.pc + 3) & 0xFF);
		regs.sp -= 2;
		regs.pc = NN;
	});

	OP(rst, 0, {
		mem_write(regs.sp - 1, (regs.pc + 1) >> 8);
		mem_write(regs.sp - 2, (regs.pc + 1) & 0xFF);
		regs.pc = h.load_addr + (y * 8);
		regs.sp -= 2;
	});

	OP(add, 1, {
		regs.flags.h = (((regs.a & 0x0F) + (alu_val & 0x0F)) & 0x10) ==
			       0x10;
		regs.flags.c = __builtin_add_overflow(regs.a, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 0;
	});

	OP(adc, 1, {
		regs.flags.h =
			(((regs.a & 0x0F) + (alu_val & 0x0F) + regs.flags.c) &
			 0x10) == 0x10;
		uint8_t tmp;
		regs.flags.c =
			__builtin_add_overflow(regs.a, regs.flags.c, &tmp) |
			__builtin_add_overflow(tmp, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 0;
	});

	OP(sub, 1, {
		regs.flags.h = (regs.a & 0x0F) < (alu_val & 0x0F);
		regs.flags.c = __builtin_sub_overflow(regs.a, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 1;
	});

	OP(sbc, 1, {
		regs.flags.h = (regs.a & 0x0F) < (alu_val & 0x0F) ||
			       (regs.a & 0x0F) < regs.flags.c;
		uint8_t tmp;
		regs.flags.c =
			__builtin_sub_overflow(regs.a, regs.flags.c, &tmp) |
			__builtin_sub_overflow(tmp, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 1;
	});

	OP(and, 1, {
		regs.flags.h = 1;
		regs.flags.n = regs.flags.c = 0;
		regs.a &= alu_val;
		regs.flags.z = !regs.a;
	});

	OP(xor, 1, {
		regs.flags.h = regs.flags.n = regs.flags.c = 0;
		regs.a ^= alu_val;
		regs.flags.z = !regs.a;
	});

	OP(or, 1, {
		regs.flags.h = regs.flags.n = regs.flags.c = 0;
		regs.a |= alu_val;
		regs.flags.z = !regs.a;
	});

	OP(cp, 1, {
		uint8_t tmp;
		regs.flags.h = (regs.a & 0x0F) < (alu_val & 0x0F);
		regs.flags.c = __builtin_sub_overflow(regs.a, alu_val, &tmp);
		regs.flags.z = tmp == 0;
		regs.flags.n = 1;
	});

	OP(rlc, 0, {
		regs.flags.c = R_READ(z) >> 7;
		R_WRITE(z, (R_READ(z) << 1) | regs.flags.c);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(rrc, 0, {
		regs.flags.c = R_READ(z) & 1;
		R_WRITE(z, (R_READ(z) >> 1) | regs.flags.c << 7);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(rl, 0, {
		size_t newc = R_READ(z) >> 7;
		R_WRITE(z, (R_READ(z) << 1) | regs.flags.c);
		regs.flags.c = newc;
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(rr, 0, {
		size_t newc = R_READ(z) & 1;
		R_WRITE(z, (R_READ(z) >> 1) | regs.flags.c << 7);
		regs.flags.c = newc;
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(sla, 0, {
		regs.flags.c = R_READ(z) >> 7;
		R_WRITE(z, R_READ(z) << 1);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(sra, 0, {
		regs.flags.c = R_READ(z) & 1; // ????
		R_WRITE(z, ((int8_t)R_READ(z)) >> 1);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(swap, 0, {
		uint8_t tmp = ((R_READ(z) & 0xF) << 4) | (R_READ(z) >> 4);
		R_WRITE(z, tmp);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = regs.flags.c = 0;
	});

	OP(srl, 0, {
		regs.flags.c = R_READ(z) & 1;
		R_WRITE(z, R_READ(z) >> 1);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});
end:;
}

static void process_cpu(void)
{
        while (regs.sp != h.sp)
                cpu_step();

        regs.pc = h.play_addr;
        regs.sp -= 2;
}

#ifndef _WIN32
static struct termios old_term;
static void restore_terminal(void)
{
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
}

static void setup_terminal(void)
{
        struct termios new_term;
        tcgetattr(STDIN_FILENO, &old_term);
        new_term = old_term;
        new_term.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        atexit(restore_terminal);
}
#else
static void setup_terminal(void) {}
static void restore_terminal(void) {}
#endif

static int poll_key(void)
{
#ifdef _WIN32
        if (_kbhit())
                return _getch();
        return -1;
#else
        int c = getchar();
        if (c != EOF)
                return c;
        return -1;
#endif
}

static size_t find_line(size_t line)
{
        for (size_t i = 0; i < playlist_len; ++i)
                if (playlist[i].line_no >= line)
                        return i;
        return playlist_len;
}

static void wait_seconds(uint32_t seconds)
{
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!stop_flag) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                if ((uint32_t)(now.tv_sec - start.tv_sec) >= seconds)
                        break;
                int c = poll_key();
                if (c == 'q') {
                        stop_flag = 1;
                        break;
                }
                if (c == 'n') {
                        next_flag = 1;
                        break;
                }
        }
}

static int parse_playlist(const char *path)
{
        FILE *pf = fopen(path, "r");
        if (!pf) {
                fprintf(stderr, "Error opening playlist file: %s\n",
                        strerror(errno));
                return -1;
        }

        char   line[256];
        size_t line_no = 0;
        size_t cap     = 0;

        while (fgets(line, sizeof(line), pf)) {
                char *p;
                line_no++;
                for (p = line; isspace((unsigned char)*p); ++p)
                        ;
                if (*p == '#' || *p == '\0' || *p == '\n')
                        continue;
                if (playlist_len >= cap) {
                        cap = cap ? cap * 2 : 16;
                        struct playlist_entry *tmp =
                                realloc(playlist, cap * sizeof(*playlist));
                        if (!tmp) {
                                fclose(pf);
                                fprintf(stderr, "Out of memory parsing playlist\n");
                                return -1;
                        }
                        playlist = tmp;
                }

                if (*p == 'T') {
                        unsigned int t;
                        unsigned long d;
                        if (sscanf(p + 1, "%u,%lu", &t, &d) != 2) {
                                fprintf(stderr,
                                        "Invalid track entry at line %zu\n",
                                        line_no);
                                continue;
                        }
                        playlist[playlist_len].type          = PL_TRACK;
                        playlist[playlist_len].line_no       = line_no;
                        playlist[playlist_len].data.track.track = (uint16_t)t;
                        playlist[playlist_len].data.track.duration = (uint32_t)d;
                        playlist_len++;
                } else if (*p == 'J') {
                        unsigned int l;
                        int          c;
                        if (sscanf(p + 1, "%u,%d", &l, &c) != 2) {
                                fprintf(stderr,
                                        "Invalid jump entry at line %zu\n",
                                        line_no);
                                continue;
                        }
                        playlist[playlist_len].type           = PL_JUMP;
                        playlist[playlist_len].line_no        = line_no;
                        playlist[playlist_len].data.jump.line = (uint16_t)l;
                        playlist[playlist_len].data.jump.count = (int16_t)c;
                        playlist[playlist_len].data.jump.executed = 0;
                        playlist_len++;
                } else {
                        fprintf(stderr, "Unknown instruction at line %zu\n",
                                line_no);
                }
        }

        fclose(pf);
        return 0;
}

static void run_playlist(void)
{
        size_t idx = 0;
        while (idx < playlist_len && !stop_flag) {
                struct playlist_entry *e = &playlist[idx];
                if (e->type == PL_TRACK) {
                        if (e->data.track.track >= h.song_count)
                                fprintf(stderr,
                                        "Warning: track %u out of range\n",
                                        e->data.track.track);
                        regs.a = e->data.track.track;
                        regs.sp = h.sp - 2;
                        regs.pc = h.init_addr;
                        fprintf(stdout, "Song %u for %u seconds\n",
                                e->data.track.track, e->data.track.duration);
                        wait_seconds(e->data.track.duration);
                        idx++;
                        if (next_flag) {
                                next_flag = 0;
                                continue;
                        }
                } else {
                        if (e->data.jump.count == -1 ||
                            e->data.jump.executed < e->data.jump.count) {
                                if (e->data.jump.count != -1)
                                        e->data.jump.executed++;
                                idx = find_line(e->data.jump.line);
                        } else {
                                idx++;
                        }
                }
        }
}

#ifdef AUDIO_DRIVER_MINIAUDIO
void miniaudio_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
        (void) pDevice;
        (void) pInput;
        (void) frameCount;

	process_cpu();
	audio_callback(&ctx, pOutput);
}
#endif
#if defined(AUDIO_DRIVER_SDL)
void sdl2_audio_callback(void *userdata, uint8_t *stream, int len)
{
	process_cpu();
	audio_callback(&ctx, (void *)stream);
}
#endif

int main(int argc, char **argv)
{
        FILE *f;
        uint_least8_t song_no = 0;
        const char *playlist_path = NULL;
        const char *song_arg      = NULL;
        const char *file_path     = NULL;

        for (int i = 1; i < argc; ++i) {
                if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                        playlist_path = argv[++i];
                } else if (!file_path) {
                        file_path = argv[i];
                } else if (!song_arg) {
                        song_arg = argv[i];
                } else {
                        fprintf(stderr,
                                "Usage: %s file [song index] [-p playlist]\n",
                                argv[0]);
                        exit(EXIT_FAILURE);
                }
        }

        if (!file_path) {
                fprintf(stderr,
                        "Usage: %s file [song index] [-p playlist]\n",
                        argv[0]);
                exit(EXIT_FAILURE);
        }

        f = fopen(file_path, "rb");
	if (!f) {
		fprintf(stderr, "Error opening file: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (fread(&h, sizeof(h), 1, f) != 1) {
		fprintf(stderr, "Error reading file: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (strncmp(h.id, "GBS", 3) != 0) {
		fprintf(stderr, "Error: Not a GBS file.\n");
		exit(EXIT_FAILURE);
	}

        if (h.version != 1) {
                fprintf(stderr, "Error: Only GBS version 1 is supported.\n");
                exit(EXIT_FAILURE);
        }

        if (playlist_path && parse_playlist(playlist_path) != 0)
                return EXIT_FAILURE;

        if (playlist_path)
                setup_terminal();

        signal(SIGINT, sigint_handler);

        /* Get user selected song number to begin playing. */
        song_no = song_arg ? atoi(song_arg) : MAX(0, h.start_song - 1);

	/* Check that user selected song number is within range of the number of
	 * songs available in input GBS file. */
	if (song_no >= h.song_count) {
		fprintf(stderr,
			"Error: The selected song index of %d is out of range. "
			"This file has %d songs.\n",
			song_no, h.song_count - 1U);
		exit(EXIT_FAILURE);
	}

	/* Allocate required memory space for playing GBS file. */
	mem = malloc((RAM_STOP_ADDR - RAM_START_ADDR) + 1);
	if (mem == NULL) {
		fprintf(stderr, "Error: malloc failure at %d.\n", __LINE__);
		exit(EXIT_FAILURE);
	}

	hram = calloc((HRAM_STOP_ADDR - HRAM_START_ADDR) + 1, 1);
	if (hram == NULL) {
		fprintf(stderr, "Error: malloc failure at %d.\n", __LINE__);
		exit(EXIT_FAILURE);
	}

	fseek(f, 0, SEEK_END);
	fseek(f, 0x70, SEEK_SET);

	banks[0] = NULL;

	uint_least8_t bno = h.load_addr / ROM_BANK_SIZE;
	uint_least16_t off = h.load_addr % ROM_BANK_SIZE;

	/* Read all ROM banks */
	while (1) {
		uint8_t *page;

		if ((page = malloc(ROM_BANK_SIZE)) == NULL) {
			fprintf(stderr, "Error: malloc failure at %d.\n",
					__LINE__);
			exit(EXIT_FAILURE);
		}

		banks[bno] = page;
                size_t rb = fread(page + off, 1, ROM_BANK_SIZE - off, f);
                (void)rb;

		if (feof(f))
			break;
		else if (ferror(f)) {
			fprintf(stderr, "Error: file read failure at %d.\n",
					__LINE__);
			exit(EXIT_FAILURE);
		}

		off = 0;
		if (++bno >= 32) {
			fprintf(stderr, "Error: too many banks in GBS file.\n");
			exit(EXIT_FAILURE);
		}
	}

	/* Close input file after loading file. */
	fclose(f);

	/* Initialising the selected ROM bank to the default of Bank 1. */
	selected_rom_bank = banks[1];

	/* Initialise CPU registers. */
	memset(&regs, 0, sizeof(regs));

	if(banks[0] == NULL)
		banks[0] = malloc(ROM_BANK_SIZE);

	if(h.load_addr >= ROM_BANK1_ADDR)
		memcpy(banks[0], &banks[1][h.load_addr - ROM_BANK1_ADDR], 0x62);
	else
		memcpy(banks[0], &banks[0][h.load_addr], 0x62);

	regs.sp = h.sp - 2;
	regs.pc = h.init_addr;
	regs.a  = song_no;

	/* TODO: Check if removing this breaks anything. */
	//mem[0xffff] = 1; // IE

	audio_init(&ctx);

	/* Load timer values from file. */
	audio_write(&ctx, 0xff06, h.tma);
	audio_write(&ctx, 0xff07, h.tac);

#if defined(AUDIO_DRIVER_SDL)
	/* Initialise SDL audio. */
	{
		SDL_AudioDeviceID audio;
		SDL_AudioSpec     got;
		SDL_AudioSpec     want = {
			    .freq     = AUDIO_SAMPLE_RATE,
			    .channels = 2,
			    .samples  = AUDIO_SAMPLES,
			    .format   = AUDIO_S16SYS,
			    .callback = sdl2_audio_callback,
		};

		if (SDL_Init(SDL_INIT_AUDIO) != 0) {
			fprintf(stderr, "Error: SDL_Init failure: %s\n",
				SDL_GetError());
			exit(EXIT_FAILURE);
		}

		if ((audio = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0)) == 0)
		{
			fprintf(stderr, "Error: SDL_OpenAudioDevice failure: "
					"%s.\n",
					SDL_GetError());
			exit(EXIT_FAILURE);
		}

		/* Begin playing audio. */
		SDL_PauseAudioDevice(audio, 0);
	}
#elif defined(AUDIO_DRIVER_MINIAUDIO)
	ma_device_config conf = ma_device_config_init(ma_device_type_playback);
	ma_device device;
	conf.playback.format = ma_format_s16;
	conf.playback.channels = 2;
	conf.sampleRate = AUDIO_SAMPLE_RATE;
	conf.dataCallback = miniaudio_callback;
	conf.periodSizeInFrames = AUDIO_SAMPLES;

	{
		if(ma_device_init(NULL,&conf, &device) != MA_SUCCESS){
			fprintf(stderr, "Miniaudio initialisation failed.\n");
			exit(1);
		}

		ma_device_start(&device);
	}
#elif defined(AUDIO_DRIVER_NONE)
	uint16_t *samples = malloc(AUDIO_SAMPLE_RATE * sizeof(uint16_t));
#else
#error "No audio driver defined."
#endif

        if (playlist_path) {
                run_playlist();
        } else {
                fprintf(stdout, "Keys: q = Quit, n = Next, p = Previous\n");
                while (!stop_flag) {
                        switch (getchar()) {
                        case 'q':
                                stop_flag = 1;
                                break;

                        case 'n':
                                if (song_no < h.song_count - 1U) {
                                        regs.a  = ++song_no;
                                        regs.sp = h.sp - 2;
                                        regs.pc = h.init_addr;
                                        fprintf(stdout, "Song %d of %d\n", song_no,
                                                h.song_count - 1U);
                                }
                                break;

                        case 'p':
                                if (song_no > 0) {
                                        regs.a  = --song_no;
                                        regs.sp = h.sp - 2;
                                        regs.pc = h.init_addr;
                                        fprintf(stdout, "Song %d of %d\n", song_no,
                                                h.song_count - 1U);
                                }
                                break;
                        }
#if defined(AUDIO_DRIVER_NONE)
                        audio_callback(NULL, (uint8_t *)samples,
                                      AUDIO_SAMPLE_RATE * sizeof(uint16_t));
#endif
                }
        }

#if defined(AUDIO_DRIVER_SDL)
	SDL_Quit();
#elif defined(AUDIO_DRIVER_MINIAUDIO)
	ma_device_uninit(&device);
#elif defined(AUDIO_DRIVER_NONE)
	free(samples);
#endif

	do {
		free(banks[bno]);
	} while(bno--);

        if (playlist_path)
                restore_terminal();

        free(mem);
        free(hram);
        free(playlist);

        return EXIT_SUCCESS;
}
