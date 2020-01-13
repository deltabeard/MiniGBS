#include "minigbs.h"
#include "audio.h"

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AUDIO_DRIVER_SDL
#include <SDL2/SDL.h>
#endif

#ifdef AUDIO_DRIVER_SOKOL
#define SOKOL_IMPL
#include "sokol_audio.h"
#endif

#ifdef AUDIO_DRIVER_MINIAL
#define MINI_AL_IMPLEMENTATION
#include "mini_al.h"
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

#include <signal.h>

static volatile int keepRunning = 1;

void intHandler(int dummy) {
	keepRunning = 0;
}


enum gbs_instr_e {
	GBS_SET_VAL = 0,
	GBS_RET,
};

/**
 * Instructions:
 *
 * SET	Set 8-bit value v at address offset a.
 * 	Address is 0xFF06 + a.
 * 0aaa aaaa vvvv vvvv
 *
 * BLK	Block set number n values v starting at address offset a.
 * 	Only useful for setting 3 or more consecutive addresses.
 * 10nn nnnn 0aaa aaaa vvvv vvvv vvvv vvvv ...
 *
 * GOTO Goto memory pointed to in slot m. Instructions at slot m must end with
 * 	BEAT. Calling a memory slot that doesn't exist is undefined.
 * 1100 mmmm
 *
 * BEAT	Returns from instructions setting musical beat and skips n number of
 *	beats.
 * 1101 nnnn
 *
 * Rejected:
 * STOR	Store the next instruction in memory slot m.
 * 	Can be used when the same set of instructions are called very
 * 	frequently. It is expected that a BLK instruction is stored in memory,
 * 	and therefore, up to 64 bytes may be used at most in each memory slot.
 * 	Storing to a memory slot overwrites any data previously written to the
 * 	slot.
 * 	TODO: Check if 16 memory slots is good or not. Maybe less is better?
 * 1100 mmmm ...
 *
 * CALL	Call instruction at memory slot m.
 * 	Calling a memory slot with no data is undefined behaviour.
 * 1101 mmmm
 */

/**
 * pGBS header:
 *
 * id is always "pGBS".
 * version is 0.
 * loop_at File offset to start looping from when reached the end of the file.
 * 	0 for no loop. 1 for first byte of file.
 * 	Values set to less than 140, that are not 0, is undefined.

struct pGBSHeader {
	char     id[4];
	uint8_t  version;
	uint16_t loop_at;
	uint8_t  tma;
	uint8_t  tac;
	char     title[32];	// Name of track
	char     author[32];	// Name of author
	char     album[32];	// Name of album/game
	char     copyright[32];	// Copyright License
	uint8_t  common[128];   // Initial RAM containing up to 16 memory slots
				// each separated by a RET instruction.
} __attribute__((packed)) pGBSHeader;

 */

struct decoded_gbs_s {
	uint8_t value;
	/* Address + 0xFF06 */
	unsigned char address : 7;
	/* 0 for set, 1 for ret. */
	unsigned char instr : 1;
}__attribute__((packed, aligned(1)));

struct record_gbs_ret_s {
	void *mem;
	size_t sz;
};

struct GBSHeader_s {
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

static struct GBSHeader_s h;
static uint8_t *	banks[32];
static uint8_t *	selected_rom_bank;

static char *instr_txt = NULL;
static size_t instr_txt_sz = 0;
static size_t instr_txt_alloc_sz = 0;

static uint8_t *pgbs_bin = NULL;
static size_t pgbs_bin_sz = 2;
static size_t pgbs_bin_alloc_sz = 0;

#define AUDIO_MEM_SIZE (0xFF3F - 0xFF00 + 1)
static uint8_t audio_mem_current[AUDIO_MEM_SIZE];

static void record_gbs_instr(const enum gbs_instr_e instr, const uint16_t addr,
			     const uint8_t val)
{
	/* If running out of memory for instructions, expand allocated memory.
	 */
	if((pgbs_bin_alloc_sz - pgbs_bin_sz) < 16)
	{
		pgbs_bin_alloc_sz += 1000;
		pgbs_bin = realloc(pgbs_bin, pgbs_bin_alloc_sz);
	}

	if(instr_txt == NULL || (instr_txt_alloc_sz - instr_txt_sz) < 128)
	{
		instr_txt_alloc_sz += 1000;
		instr_txt = realloc(instr_txt, instr_txt_alloc_sz);
	}

	if(instr_txt_sz == 0)
		instr_txt[0] = '\0';

	assert(pgbs_bin != NULL);
	assert(instr_txt != NULL);

	static char instr_str[32];

	// TODO: decide best way to organise addresses for faster channel
	// selection, instead of division by 5.
	switch(instr)
	{
	case GBS_SET_VAL:
		/* Make sure address is positive. */
		assert((((signed long) addr) - 0xFF00) >= 0);
		assert(addr <= 0xFF3F);
		switch(addr)
		{
		case 0xFF06 ... 0xFF07:
		case 0xFF10 ... 0xFF14:
		case 0xFF16 ... 0xFF19:
		case 0xFF1A ... 0xFF1E:
		case 0xFF30 ... 0xFF3F:
		case 0xFF20 ... 0xFF26:
			break;

		/* Ignoring invalid addresses. */
		default:
			strcat(instr_txt, "IGN\n");
			instr_txt_sz += 4;
			return;
		}

		pgbs_bin[pgbs_bin_sz] = addr - 0xFF00;
		pgbs_bin[pgbs_bin_sz + 1] = val;
		pgbs_bin_sz += 2;

		sprintf(instr_str, "SET %#06x %#04x\n", addr, val);
		strcat(instr_txt, instr_str);
		instr_txt_sz += strlen(instr_str);

		break;

	case GBS_RET:
		pgbs_bin[pgbs_bin_sz] = 0b11010000;
		pgbs_bin_sz += 1;

		strcat(instr_txt, "RET\n");
		instr_txt_sz += 4;

		break;

	default:
		abort();
	}

	return;
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
	{
		audio_mem_current[addr - 0xFF00] = val;
		if(audio_read(addr - 0xFF00) != val)
			record_gbs_instr(GBS_SET_VAL, addr, val);

		audio_write(addr - 0xFF00, val);
#if 0
		/* Always set "Initial" and Channel Control registers. */
		switch(addr)
		{
			case 0x12:
			case 0x14:
			case 0x17:
			case 0x19:
			case 0x1A:
			case 0x1E:
			case 0x23:
			case 0x24:
			case 0x25:
				record_gbs_instr(GBS_SET_VAL, addr, val);
				break;
		}
#endif
	}
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
		return audio_read(addr - 0xFF00);
	else if (addr >= HRAM_START_ADDR && addr <= HRAM_STOP_ADDR)
		return hram[addr - HRAM_START_ADDR];

	/* Catch-all for everything else. */
	return 0xFF;
}

#if 1
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
#endif

void process_cpu(void)
{
	static unsigned int first_ret = 0;
	static uint8_t audio_mem_last[AUDIO_MEM_SIZE];
	memcpy(audio_mem_last, audio_mem_current, AUDIO_MEM_SIZE);

	while (regs.sp != h.sp)
		cpu_step();

	regs.pc = h.play_addr;
	regs.sp -= 2;

#if 0
	static uint8_t valid_audio_regs[] = {
		0x06, 0x07,
		0x10, 0x11, 0x12, 0x14,
		0x16, 0x17, 0x18, 0x19,
		0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
		0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
		0x20, 0x21, 0x22, 0x23,
		0x24, 0x25, 0x26
	};

	/* Find changes in audio registers. */
	for(uint_least8_t i = 0; i < sizeof(valid_audio_regs); i++)
	{
		if(first_ret == 0 || audio_mem_last[i] != audio_mem_current[i])
		{
			record_gbs_instr(GBS_SET_VAL, i + 0xFF00,
					 audio_mem_current[i]);
		}
	}
#endif

	record_gbs_instr(GBS_RET, 0, 0);
	first_ret = 1;
}

#ifdef AUDIO_DRIVER_SOKOL
void sokol_audio_callback(float* buffer, int num_frames, int num_channels)
{
	audio_callback(NULL, (uint8_t *)buffer, num_frames * num_channels * sizeof(float));
}
#endif

#ifdef AUDIO_DRIVER_MINIAL
mal_uint32 minial_audio_callback(mal_device* pDevice, mal_uint32 frameCount, void* pSamples)
{
	const uint_least8_t channels = 2;
	(void)pDevice;

	audio_callback(NULL, (uint8_t *)pSamples, frameCount * channels * sizeof(float));
	return frameCount;
}
#endif

int main(int argc, char **argv)
{
	FILE *f;
	uint_least8_t song_no;
	unsigned long play_time = 0;

	if (argc != 2 && argc != 3 && argc != 4)
	{
		fprintf(stderr, "Usage: %s file [song index] [play time]\n",
argv[0]);
		exit(EXIT_FAILURE);
	}

	f = fopen(argv[1], "rb");
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

	/* Get user selected song number to begin playing. */
	song_no = argc > 2 ? atoi(argv[2]) : MAX(0, h.start_song - 1);

	if(argc == 4)
		play_time = atoi(argv[3]);

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
		fread(page + off, 1, ROM_BANK_SIZE - off, f);

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

	printf("Title: %s\n"
		"Author: %s\n"
		"Copyright: %s\n",
		h.title, h.author, h.copyright);

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

	/* Load timer values from file. */
	audio_write(0x06, h.tma);
	audio_write(0x07, h.tac);

	pgbs_bin = realloc(pgbs_bin, 1000);
	pgbs_bin_alloc_sz = 1000;
	pgbs_bin[0] = h.tma;
	pgbs_bin[1] = h.tac;
	audio_init();

#if defined(AUDIO_DRIVER_SDL)
	/* Initialise SDL audio. */
	{
		SDL_AudioDeviceID audio;
		SDL_AudioSpec     got;
		SDL_AudioSpec     want = {
			    .freq     = AUDIO_SAMPLE_RATE,
			    .channels = 2,
			    .samples  = AUDIO_SAMPLE_RATE / 12U,
			    .format   = AUDIO_F32SYS,
			    .callback = audio_callback,
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
#elif defined(AUDIO_DRIVER_SOKOL)
	/* Initialise SOKOL Audio. */
	{
		const saudio_desc sd = {
			.stream_cb = sokol_audio_callback,
			.sample_rate = AUDIO_SAMPLE_RATE,
			.num_channels = 2

		};
		saudio_setup(&sd);
	}
#elif defined(AUDIO_DRIVER_MINIAL)
	mal_device device;
	mal_context audio_ctx;
	mal_device_config config;

	{
		if(mal_context_init(NULL, 0, NULL, &audio_ctx) != MAL_SUCCESS){
			fprintf(stderr, "mal_context_init failed.\n");
			exit(1);
		}

		config = mal_device_config_init_playback(
				mal_format_f32, 2, AUDIO_SAMPLE_RATE,
				minial_audio_callback
		);

		if (mal_device_init(NULL, mal_device_type_playback, NULL, &config, NULL, &device) != MAL_SUCCESS) {
			printf("Failed to open playback device.\n");
			return -3;
		}

		if (mal_device_start(&device) != MAL_SUCCESS) {
			printf("Failed to start playback device.\n");
			mal_device_uninit(&device);
			return -4;
		}
	}
#elif defined(AUDIO_DRIVER_NONE)
	float *samples = malloc(AUDIO_SAMPLE_RATE * sizeof(float));
	unsigned long sec = 0;
#else
#error "No audio driver defined."
#endif

	/* Fixes printf's not printing to stdout until exit in Windows. */
	setbuf(stdout, NULL);

	signal(SIGINT, intHandler);

	puts("CTRL+C or SIGINT to exit.");

	while (keepRunning && (play_time == 0 || sec < play_time))
	{
#if defined(AUDIO_DRIVER_NONE)
		audio_callback(NULL, (uint8_t *)samples, AUDIO_SAMPLE_RATE *
sizeof(float));
		sec++;
#endif
	}

out:
#if defined(AUDIO_DRIVER_SDL)
	SDL_Quit();
#elif defined(AUDIO_DRIVER_SOKOL)
	saudio_shutdown();
#elif defined(AUDIO_DRIVER_MINIAL)
	mal_device_uninit(&device);
#elif defined(AUDIO_DRIVER_NONE)
	free(samples);
#endif

	audio_deinit();

	do {
		free(banks[bno]);
	} while(bno--);

	{
		FILE *dgbs = fopen("dgbs", "wb");
		fwrite(pgbs_bin, 1, pgbs_bin_sz, dgbs);
		fclose(dgbs);
		printf("\nWrote %ld bytes.\n", pgbs_bin_sz);
		free(pgbs_bin);

		dgbs = fopen("dgbs.txt", "w");
		fputs(instr_txt, dgbs);
		fclose(dgbs);
		free(instr_txt);
	}

	free(mem);
	free(hram);

	return EXIT_SUCCESS;
}
