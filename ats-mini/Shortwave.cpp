#include "Common.h"
#include "Menu.h"
#include "Shortwave.h"

struct ShortwaveRange
{
  uint16_t minimum;
  uint16_t maximum;
  const char *meterBand;
};

// ITU Region 3 allocations from 11m through 120m. Keep the complete 60m
// receiver band contiguous so standard-frequency stations around 5000kHz are
// included as well.
static constexpr ShortwaveRange shortwaveRanges[] =
{
  {  2300,  2495, "120m" },
  {  3200,  3400,  "90m" },
  {  3900,  4000,  "75m" },
  {  4750,  5060,  "60m" },
  {  5900,  6200,  "49m" },
  {  7200,  7450,  "41m" },
  {  9400,  9900,  "31m" },
  { 11600, 12100,  "25m" },
  { 13570, 13870,  "22m" },
  { 15100, 15800,  "19m" },
  { 17480, 17900,  "16m" },
  { 18900, 19020,  "15m" },
  { 21450, 21850,  "13m" },
  { 25670, 26100,  "11m" },
};

bool shortwaveBroadcastOnly()
{
  const Band *band = getCurrentBand();
  return currentMode == AM && band->bandType == SW_BAND_TYPE &&
         band->minimumFreq >= shortwaveRanges[0].minimum &&
         band->maximumFreq <= shortwaveRanges[LAST_ITEM(shortwaveRanges)].maximum;
}

bool shortwaveBroadcastRange(uint16_t frequency, int16_t direction,
                             uint16_t *minimum, uint16_t *maximum)
{
  if(!shortwaveBroadcastOnly() || !direction) return false;

  const Band *band = getCurrentBand();
  if(direction > 0)
  {
    for(const ShortwaveRange &range : shortwaveRanges)
    {
      uint16_t low = max(range.minimum, band->minimumFreq);
      uint16_t high = min(range.maximum, band->maximumFreq);
      if(low <= high && frequency <= high)
      {
        if(minimum) *minimum = low;
        if(maximum) *maximum = high;
        return true;
      }
    }
  }
  else
  {
    for(int i = LAST_ITEM(shortwaveRanges); i >= 0; --i)
    {
      uint16_t low = max(shortwaveRanges[i].minimum, band->minimumFreq);
      uint16_t high = min(shortwaveRanges[i].maximum, band->maximumFreq);
      if(low <= high && frequency >= low)
      {
        if(minimum) *minimum = low;
        if(maximum) *maximum = high;
        return true;
      }
    }
  }
  return false;
}

uint16_t shortwaveBroadcastFrequency(int32_t frequency, int16_t direction)
{
  uint16_t minimum, maximum;
  if(direction > 0)
  {
    if(shortwaveBroadcastRange(frequency, direction, &minimum, &maximum))
      return max((int32_t)minimum, frequency);
    shortwaveBroadcastRange(0, direction, &minimum, &maximum);
    return minimum;
  }

  if(shortwaveBroadcastRange(frequency, direction, &minimum, &maximum))
    return min((int32_t)maximum, frequency);
  shortwaveBroadcastRange(UINT16_MAX, direction, &minimum, &maximum);
  return maximum;
}

const char *shortwaveMeterBand(uint16_t frequency)
{
  for(const ShortwaveRange &range : shortwaveRanges)
    if(frequency >= range.minimum && frequency <= range.maximum)
      return range.meterBand;
  return nullptr;
}
