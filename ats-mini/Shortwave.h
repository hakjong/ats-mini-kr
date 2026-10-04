#ifndef SHORTWAVE_H
#define SHORTWAVE_H

#include <stdint.h>

// True when the current AM band is wholly within the 11m-120m shortwave
// broadcast range. This excludes ALL and CB while covering the individual
// broadcast bands and the unified SW band.
bool shortwaveBroadcastOnly();

// Find the current or next ITU Region 3 broadcast allocation, clipped to the
// current receiver band. Direction must be positive or negative.
bool shortwaveBroadcastRange(uint16_t frequency, int16_t direction,
                             uint16_t *minimum, uint16_t *maximum);

// Move a VF tuning candidate out of allocation gaps, wrapping at the ends of
// the current receiver band.
uint16_t shortwaveBroadcastFrequency(int32_t frequency, int16_t direction);

// Return the conventional meter-band name for an allocated frequency.
const char *shortwaveMeterBand(uint16_t frequency);

#endif // SHORTWAVE_H
