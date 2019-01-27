#include <stdint.h>

extern void process_cpu(void);
extern void mem_write(const uint16_t addr, const uint8_t val);
extern uint8_t mem_read(const uint16_t addr);
