#include <stdint.h>

#define AUDIO_SAMPLE_RATE 48000.0f

void    audio_callback(void *ptr, uint8_t *data, int len);
void    audio_update(void);
uint8_t audio_read(const uint16_t addr);
void    audio_write(const uint16_t addr, const uint8_t val);
void    audio_init(void);
