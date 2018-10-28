#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include "minigbs.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Some of the bitfield / casting used in here assumes little endian :("
#endif

uint8_t* mem;

static uint8_t* banks[32];
static struct GBSHeader h;

void bank_switch(int which)
{
	// allowing bank switch to 0 seems to break some games
	if(which > 0 && which < 32 && banks[which]){
		memcpy(mem + 0x4000, banks[which], 0x4000);
	}
}

static inline void mem_write(uint16_t addr, uint8_t val)
{
	if(addr >= 0x2000 && addr < 0x4000){
		bank_switch(val);
	} else if(addr >= 0xFF10 && addr <= 0xFF40){
		audio_write(addr, val);
	} else if(addr < 0x8000){
	} else if(addr == 0xFF06 || addr == 0xFF07){
		mem[addr] = val;
		audio_update_rate();
	} else {
		mem[addr] = val;
	}
}

static inline uint8_t mem_read(uint16_t addr){
	uint8_t val = mem[addr];

	static uint8_t ortab[] = {
		0x80, 0x3f, 0x00, 0xff, 0xbf,
		0xff, 0x3f, 0x00, 0xff, 0xbf,
		0x7f, 0xff, 0x9f, 0xff, 0xbf,
		0xff, 0xff, 0x00, 0x00, 0xbf,
		0x00, 0x00, 0x70
	};
	
	if(addr >= 0xFF10 && addr <= 0xFF26){
		val |= ortab[addr - 0xFF10];
	}

	return val;
}

void cpu_step(void)
{

	uint8_t op = mem[regs.pc];
	size_t x = op >> 6;
	size_t y = (op >> 3) & 7;
	size_t z = op & 7;

	unsigned cycles = 0;

#define OP(x) &&op_##x
#define ALUY (void*)(1)

	static const void* xmap[4] = {
		[1] = OP(mov8),
		[2] = ALUY
	};

	static const void* zmap[4][8] = {
		[0] = {
			[2] = OP(ldsta16),
			[3] = OP(incdec16),
			[4] = OP(inc8),
			[5] = OP(dec8),
			[6] = OP(ld8)
		},
		[3] = {
			[6] = ALUY,
			[7] = OP(rst)
		},
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

	static const void* alu[8] = {
		OP(add), OP(adc), OP(sub), OP(sbc), OP(and), OP(xor), OP(or), OP(cp)
	};

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
	uint8_t*           r[] = { &regs.b, &regs.c, &regs.d, &regs.e, &regs.h, &regs.l, mem + regs.hl, &regs.a };
	static uint16_t*  rr[] = { &regs.bc, &regs.de, &regs.hl, &regs.hl };
	static void*     rot[] = { &&op_rlc, &&op_rrc, &&op_rl, &&op_rr, &&op_sla, &&op_sra, &&op_swap, &&op_srl };
	static uint16_t* rp2[] = { &regs.bc, &regs.de, &regs.hl, &regs.af };

	uint8_t alu_val;

#define R_READ(i)     ({ uint8_t v; if(i == 6){ v = mem_read(regs.hl); } else { v = *r[i]; } v; })
#define R_WRITE(i, v) ({ if(i == 6){ mem_write(regs.hl, v); } else { *r[i] = v; }; *r[i]; })

	if(xmap[x] > ALUY){
		goto *xmap[x];
	} else if(xmap[x] == ALUY){
		alu_val = R_READ(z);
		goto *alu[y];
	} else if(zmap[x][z] > ALUY){
		goto *zmap[x][z];
	} else if(zmap[x][z] == ALUY){
		alu_val = mem_read(++regs.pc);
		goto *alu[y];
	} else {
		goto *ymap[x][z][y];
	}

#undef ALUY
#undef OP

#define OP(name, len, cy, code) op_##name: { code; cycles += cy; regs.pc += len; goto end; }
#define CHECKCC(n) (((regs.flags.all >> cc[n].shift) & 1) == cc[n].want)

#define SS(p) (((uint16_t*)&regs.bc)[p])
#define DD(p) (((uint16_t*)&regs.bc)+(p))
#define NN    ((((uint16_t)mem_read(regs.pc+2)) << 8) | mem_read(regs.pc+1))

	OP(mov8, 1, 4, {
		if(z == 6 && y == 6){
			puts("HALT?");
		} else {
			if(z == 6 || y == 6){ cycles += 4; }
			R_WRITE(y, R_READ(z));
		}
	});

	OP(ldsta16, 1, 8, {
		size_t p = y >> 1;

		if(y & 1){
			regs.a = mem_read(*rr[p]);
		} else {
			mem_write(*rr[p], regs.a);
		}

		if(p == 2) regs.hl++;
		else if(p == 3) regs.hl--;
	});

	OP(incdec16, 1, 8, {
		if(y & 1){
			--*DD(y >> 1);
		} else {
			++*DD(y >> 1);
		}
	});

	OP(inc8, 1, 4, {
		regs.flags.h = (R_READ(y) & 0xF) == 9;
		R_WRITE(y, R_READ(y) + 1);
		regs.flags.z = !R_READ(y);
		regs.flags.n = 0;
	});

	OP(dec8, 1, 4, {
		regs.flags.h = (R_READ(y) & 0xF) == 0;
		R_WRITE(y, R_READ(y) - 1);
		regs.flags.z = !R_READ(y);
		regs.flags.n = 1;
	});

	OP(ld8, 2, 8, {
		R_WRITE(y, mem_read(regs.pc + 1));
		if(y == 6) cycles += 4;
	});

	OP(nop, 1, 4, {
		// skip
	});

	OP(stsp, 3, 20, {
		mem_write(NN+1, regs.sp >> 8);
		mem_write(NN  , regs.sp & 0xFF);
	});

	OP(stop, 2, 4, {
		// skip
	});

	OP(jr, 2, 12, {
		regs.pc += (int8_t)mem_read(regs.pc+1);
	});

	OP(jrcc, 2, 8, {
		if(CHECKCC(y - 4)){
			regs.pc += (int8_t)mem_read(regs.pc+1);
			cycles += 4;
		}
	});

	OP(ld16, 3, 8, {
		*DD(y >> 1) = NN;
	});

	OP(addhl, 1, 8, {
		uint16_t ss = SS(y >> 1);
		regs.flags.h = (((ss&0x0FFF) + (regs.hl&0x0FFF)) & 0x1000) == 0x1000;
		regs.flags.c = __builtin_add_overflow(regs.hl, ss, &regs.hl);
		regs.flags.n = 0;
	});

	OP(rlca, 1, 4, {
		regs.flags.c = regs.a >> 7;
		regs.a = (regs.a << 1) | regs.flags.c;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(rrca, 1, 4, {
		regs.flags.c = regs.a & 1;
		regs.a = (regs.a >> 1) | regs.flags.c << 7;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(rla, 1, 4, {
		size_t newc = regs.a >> 7;
		regs.a = (regs.a << 1) | regs.flags.c;
		regs.flags.c = newc;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(rra, 1, 4, {
		size_t newc = regs.a & 1;
		regs.a = (regs.a >> 1) | regs.flags.c << 7;
		regs.flags.c = newc;
		regs.flags.z = regs.flags.n = regs.flags.h = 0;
	});

	OP(daa, 1, 4, {
		size_t up = regs.a >> 4;
		size_t dn = regs.a & 0xF;
		size_t newc = 0;

		if(dn >= 10 || regs.flags.h){
			if(regs.flags.n){
				newc |= __builtin_sub_overflow(regs.a, 0x06, &regs.a);
			} else {
				newc |= __builtin_add_overflow(regs.a, 0x06, &regs.a);
			}
		}

		if(up >= 10 || regs.flags.c){
			if(regs.flags.n){
				newc |= __builtin_sub_overflow(regs.a, 0x60, &regs.a);
			} else {
				newc |= __builtin_add_overflow(regs.a, 0x60, &regs.a);
			}
		}

		regs.flags.c = newc;
		regs.flags.h = 0;
		regs.flags.z = !regs.a;
	});

	OP(cpl, 1, 4, {
		regs.a = ~regs.a;
		regs.flags.h = 1;
		regs.flags.n = 1;
	});

	OP(scf, 1, 4, {
		regs.flags.c = 1;
		regs.flags.h = 0;
		regs.flags.n = 0;
	});

	OP(ccf, 1, 4, {
		regs.flags.c = !regs.flags.c;
		regs.flags.h = 0;
		regs.flags.n = 0;
	});

	OP(retcc, 1, 8, {
		if(CHECKCC(y)){
			regs.pc = ((mem_read(regs.sp+1) << 8) | mem_read(regs.sp)) - 1;
			regs.sp += 2;
			cycles += 12;
		}
	});

	OP(sth, 2, 12, {
		mem_write(0xFF00 + mem_read(regs.pc+1), regs.a);
	});

	OP(addsp, 2, 16, {
		regs.flags.h = (((regs.sp&0x0FFF) + (mem_read(regs.pc+1)&0x0F)) & 0x1000) == 0x1000;
		regs.flags.c = __builtin_add_overflow(regs.sp, (int8_t)mem_read(regs.pc+1), (int16_t*)&regs.sp);
		regs.flags.z = regs.flags.n = 0;
	});

	OP(ldh, 2, 12, {
		regs.a = mem_read(0xFF00 + mem_read(regs.pc+1));
	});

	OP(ldsp, 2, 12, {
		regs.hl = regs.sp + (char)mem[regs.pc+1];
		regs.flags.h = regs.flags.n = regs.flags.z = regs.flags.c = 0; // XXX: probably wrong
	});

	OP(pop, 1, 12, {
		*rp2[y >> 1] = (mem_read(regs.sp+1) << 8) | mem_read(regs.sp);
		regs.sp += 2;
	});

	OP(ret, 0, 16, {
		regs.pc = (mem_read(regs.sp+1) << 8 | mem_read(regs.sp));
		regs.sp += 2;
	});

	OP(reti, 0, 16, {
		regs.pc = mem_read(regs.sp+1) << 8 | mem_read(regs.sp);
		regs.sp += 2;
		// XXX: interrupts not implemented
	});

	OP(jphl, 0, 4, {
		regs.pc = regs.hl;
	});

	OP(sphl, 1, 8, {
		regs.sp = regs.hl;
	});

	OP(jpcc, 3, 12, {
		if(CHECKCC(y)){
			regs.pc = NN - 3;
			cycles += 4;
		}
	});

	OP(stha, 1, 8, {
		mem_write(0xFF00 + regs.c, regs.a);
	});

	OP(st16, 3, 16, {
		mem_write(NN, regs.a);
	});

	OP(ldha, 1, 8, {
		regs.a = mem_read(0xFF00 + regs.c);
	});

	OP(lda16, 3, 16, {
		regs.a = mem_read(NN);
	});

	OP(jp, 0, 16, {
		regs.pc = NN;
	});

	OP(cb, 0, 0, {
		op = mem_read(++regs.pc);
		x = (op >> 6);
		y = (op >> 3) & 7;
		z = op & 7;

		cycles += (z == 6) ? 16 : 8;
		++regs.pc;

		if(x == 0){
			goto *rot[y];
		} else if(x == 1){ // BIT
			regs.flags.z = !(R_READ(z) & (1 << y));
			regs.flags.n = 0;
			regs.flags.h = 1;
		} else if(x == 2){ // RES
			R_WRITE(z, R_READ(z) & ~(1 << y));
		} else { // SET
			R_WRITE(z, R_READ(z) | (1 << y));
		}
	});

	OP(undef, 1, 4, {
		// skip
	});

	OP(di, 1, 4, {
		// XXX: interrupts not implemented
	});

	OP(ei, 1, 4, {
		// XXX: interrupts not implemented
	});

	OP(callcc, 3, 12, {
		if(CHECKCC(y)){
			mem_write(regs.sp-1, (regs.pc + 3) >> 8);
			mem_write(regs.sp-2, (regs.pc + 3) & 0xFF);
			regs.sp -= 2;
			regs.pc = NN - 3;
			cycles += 12;
		}
	});

	OP(push, 1, 16, {
		mem_write(regs.sp-2, *rp2[y >> 1] & 0xFF);
		mem_write(regs.sp-1, *rp2[y >> 1] >> 8);
		regs.sp -= 2;
	});

	OP(call, 0, 24, {
		mem_write(regs.sp-1, (regs.pc + 3) >> 8);
		mem_write(regs.sp-2, (regs.pc + 3) & 0xFF);
		regs.sp -= 2;
		regs.pc = NN;
	});

	OP(rst, 0, 16, {
		mem_write(regs.sp-1, (regs.pc + 1) >> 8);
		mem_write(regs.sp-2, (regs.pc + 1) & 0xFF);
		regs.pc = h.load_addr + (y*8);
		regs.sp -= 2;
	});

	OP(add, 1, 4, {
		regs.flags.h = (((regs.a&0x0F) + (alu_val&0x0F)) & 0x10) == 0x10;
		regs.flags.c = __builtin_add_overflow(regs.a, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 0;
	});

	OP(adc, 1, 4, {
		regs.flags.h = (((regs.a&0x0F) + (alu_val&0x0F) + regs.flags.c) & 0x10) == 0x10;
		uint8_t tmp;
		regs.flags.c = __builtin_add_overflow(regs.a, regs.flags.c, &tmp) | __builtin_add_overflow(tmp, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 0;
	});

	OP(sub, 1, 4, {
		regs.flags.h = (regs.a&0x0F) < (alu_val&0x0F);
		regs.flags.c = __builtin_sub_overflow(regs.a, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 1;
	});

	OP(sbc, 1, 4, {
		regs.flags.h = (regs.a&0x0F) < (alu_val&0x0F) || (regs.a&0x0F) < regs.flags.c;
		uint8_t tmp;
		regs.flags.c = __builtin_sub_overflow(regs.a, regs.flags.c, &tmp) | __builtin_sub_overflow(tmp, alu_val, &regs.a);
		regs.flags.z = regs.a == 0;
		regs.flags.n = 1;
	});

	OP(and, 1, 4, {
		regs.flags.h = 1;
		regs.flags.n = regs.flags.c = 0;
		regs.a &= alu_val;
		regs.flags.z = !regs.a;
	});

	OP(xor, 1, 4, {
		regs.flags.h = regs.flags.n = regs.flags.c = 0;
		regs.a ^= alu_val;
		regs.flags.z = !regs.a;
	});

	OP(or, 1, 4, {
		regs.flags.h = regs.flags.n = regs.flags.c = 0;
		regs.a |= alu_val;
		regs.flags.z = !regs.a;
	});

	OP(cp, 1, 4, {
		uint8_t tmp;
		regs.flags.h = (regs.a&0x0F) < (alu_val&0x0F);
		regs.flags.c = __builtin_sub_overflow(regs.a, alu_val, &tmp);
		regs.flags.z = tmp == 0;
		regs.flags.n = 1;
	});

	OP(rlc, 0, 0, {
		regs.flags.c = R_READ(z) >> 7;
		R_WRITE(z, (R_READ(z) << 1) | regs.flags.c);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(rrc, 0, 0, {
		regs.flags.c = R_READ(z) & 1;
		R_WRITE(z, (R_READ(z) >> 1) | regs.flags.c << 7);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(rl, 0, 0, {
		size_t newc = R_READ(z) >> 7;
		R_WRITE(z, (R_READ(z) << 1) | regs.flags.c);
		regs.flags.c = newc;
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(rr, 0, 0, {
		size_t newc = R_READ(z) & 1;
		R_WRITE(z, (R_READ(z) >> 1) | regs.flags.c << 7);
		regs.flags.c = newc;
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(sla, 0, 0, {
		regs.flags.c = R_READ(z) >> 7;
		R_WRITE(z, R_READ(z) << 1);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(sra, 0, 0, {
		regs.flags.c = R_READ(z) & 1; // ????
		R_WRITE(z, ((int8_t)R_READ(z)) >> 1);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});

	OP(swap, 0, 0, {
		uint8_t tmp = ((R_READ(z) & 0xF) << 4) | (R_READ(z) >> 4);
		R_WRITE(z, tmp);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = regs.flags.c = 0;
	});

	OP(srl, 0, 0, {
		regs.flags.c = R_READ(z) & 1;
		R_WRITE(z, R_READ(z) >> 1);
		regs.flags.z = !R_READ(z);
		regs.flags.n = regs.flags.h = 0;
	});
end:;
}

void process_cpu(void)
{
	while(regs.sp != h.sp)
		cpu_step();

	regs.pc = h.play_addr;
	regs.sp -= 2;

	audio_update();
}

int main(int argc, char** argv)
{
	FILE *f;

	if(argc != 2 && argc != 3)
	{
		fprintf(stderr, "Usage: %s file [song index]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	f = fopen(argv[1], "r");
	if(!f)
	{
		fprintf(stderr, "Error opening file: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if(fread(&h, sizeof(h), 1, f) != 1)
	{
		fprintf(stderr, "Error reading file: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if(strncmp(h.id, "GBS", 3) != 0)
	{
		fprintf(stderr, "Error: Not a GBS file.\n");
		exit(EXIT_FAILURE);
	}

	if(h.version != 1)
	{
		fprintf(stderr, "Error: Only GBS version 1 is supported.\n");
		exit(EXIT_FAILURE);
	}

	unsigned int song_no = argc > 2 ? atoi(argv[2]) : MAX(0, h.start_song - 1);
	if(song_no >= h.song_count)
	{
		fprintf(stderr,
				"Error: The selected song index of %d is out of range. "
				"This file has %d songs.\n",
				song_no, h.song_count);
		exit(EXIT_FAILURE);
	}

	/* Allocate full Game Boy memory area. */
	if((mem = malloc(0x10000)) == NULL)
	{
		printf("%d: malloc failure", __LINE__);
		exit(EXIT_FAILURE);
	}

	fseek(f, 0, SEEK_END);
	fseek(f, 0x70, SEEK_SET);

	int bno = h.load_addr / 0x4000;
	int off = h.load_addr % 0x4000;

	/* Read all ROM banks */
	while(1)
	{
		uint8_t* page;

		if((page = malloc(0x4000)) == NULL)
		{
			printf("%d: malloc failure", __LINE__);
			exit(EXIT_FAILURE);
		}

		banks[bno] = page;

		size_t n = fread(page + off, 1, 0x4000 - off, f);
		if(feof(f)){
			break;
		} else if(ferror(f)){
			printf("Error reading file: %m\n");
			break;
		}

		off = 0;
		if(++bno >= 32){
			puts("Too many banks...");
			exit(1);
		}
	}

	/* Close input file after loading file. */
	fclose(f);

	/* Load timer values from file. */
	mem[0xff06] = h.tma;
	mem[0xff07] = h.tac;

	/* Copy data to ROM banks 1 and 2. */
	if(banks[0])
		memcpy(mem, banks[0], 0x4000);

	if(banks[1])
		memcpy(mem + 0x4000, banks[1], 0x4000);

	/* Initialise the rest of the working memory area. */
	memset(mem + 0x8000, 0, 0x8000);

	/* Initialise CPU registers. */
	memset(&regs, 0, sizeof(regs));

	memcpy(mem, mem + h.load_addr, 0x62);

	regs.sp = h.sp - 2;
	regs.pc = h.init_addr;
	regs.a = song_no;

	mem[0xffff] = 1; // IE
	mem[0xff06] = h.tma;
	mem[0xff07] = h.tac;

	/* Initialise IO registers. */
	{
		const uint8_t regs_init[] = {
			0x80, 0xBF, 0xF3, 0xFF, 0x3F, 0xFF, 0x3F, 0x00,
			0xFF, 0x3F, 0x7F, 0xFF, 0x9F, 0xFF, 0x3F, 0xFF,
			0xFF, 0x00, 0x00, 0x3F, 0x77, 0xF3, 0xF1
		};

		for(int i = 0; i < 23; ++i)
			mem_write(0xFF10 + i, regs_init[i]);
	}

	/* Initialise Wave Pattern RAM. */
	{
		const uint8_t wave_init[] = {
			0xac, 0xdd, 0xda, 0x48,
			0x36, 0x02, 0xcf, 0x16,
			0x2c, 0x04, 0xe5, 0x2c,
			0xac, 0xdd, 0xda, 0x48
		};

		memcpy(mem + 0xff30, wave_init, 16);
	}

	audio_init();

	while(1)
	{
		switch(getchar())
		{
			case 'q':
				exit(EXIT_SUCCESS);
		}
	}

	return EXIT_SUCCESS;
}
