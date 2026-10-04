#include "Common.h"
#include "Draw.h"
#include "EtmPlus.h"
#include "Menu.h"
#include "Storage.h"
#include "Shortwave.h"
#include "Utils.h"

#include <stdlib.h>

#define ETM_PLUS_LIMIT   256
#define ETM_PLUS_VERSION 1

struct SavedEtmPlus
{
  uint8_t version;
  uint8_t mode;
  uint8_t hour;
  uint16_t count;
  uint16_t minimumFreq;
  uint16_t maximumFreq;
  uint16_t frequencies[ETM_PLUS_LIMIT];
};

static SavedEtmPlus *etmPlus = nullptr;
static uint8_t loadedBand = 255;
static uint8_t loadedHour = 255;
static bool scanAborted = false;
static const SavedEtmPlus *activeScan = nullptr;
static uint8_t activeScanHour = 255;
static uint16_t scanFoundCount = 0;
static uint16_t recentFound[5] = {};
static uint8_t recentCount = 0;

static bool ensureEtmPlus()
{
  if(etmPlus) return true;
  etmPlus = static_cast<SavedEtmPlus *>(ps_malloc(sizeof(*etmPlus)));
  if(etmPlus) memset(etmPlus, 0, sizeof(*etmPlus));
  return etmPlus != nullptr;
}

static void etmPlusKey(char *key, uint8_t band, uint8_t hour)
{
  snprintf(key, 16, "B%u-H%02u", band, hour);
}

bool etmPlusSupported()
{
  const Band *band = getCurrentBand();
  return currentMode == AM && band->bandType == SW_BAND_TYPE;
}

bool etmPlusCurrentHour(uint8_t *hour)
{
  return clockGetLocalHM(hour, nullptr);
}

static bool etmPlusLoad(uint8_t band, uint8_t hour)
{
  if(!ensureEtmPlus()) return false;
  const Band *current = &bands[band];
  if(loadedBand == band && loadedHour == hour && etmPlus->mode == current->bandMode &&
     etmPlus->minimumFreq == current->minimumFreq && etmPlus->maximumFreq == current->maximumFreq)
    return true;

  char key[16];
  etmPlusKey(key, band, hour);
  loadedBand = band;
  loadedHour = hour;
  memset(etmPlus, 0, sizeof(*etmPlus));

  prefs.begin("etm-plus", true, STORAGE_PARTITION);
  if(prefs.getBytesLength(key) == sizeof(*etmPlus))
    prefs.getBytes(key, etmPlus, sizeof(*etmPlus));
  prefs.end();

  if(etmPlus->version != ETM_PLUS_VERSION || etmPlus->mode != current->bandMode ||
     etmPlus->hour != hour || etmPlus->minimumFreq != current->minimumFreq ||
     etmPlus->maximumFreq != current->maximumFreq || etmPlus->count > ETM_PLUS_LIMIT)
    memset(etmPlus, 0, sizeof(*etmPlus));
  else
    for(uint16_t i = 0; i < etmPlus->count; ++i)
      if(etmPlus->frequencies[i] < current->minimumFreq ||
         etmPlus->frequencies[i] > current->maximumFreq ||
         (i && etmPlus->frequencies[i] <= etmPlus->frequencies[i - 1]))
      {
        memset(etmPlus, 0, sizeof(*etmPlus));
        break;
      }

  if(etmPlus->version != ETM_PLUS_VERSION)
  {
    etmPlus->version = ETM_PLUS_VERSION;
    etmPlus->mode = current->bandMode;
    etmPlus->hour = hour;
    etmPlus->minimumFreq = current->minimumFreq;
    etmPlus->maximumFreq = current->maximumFreq;
  }
  return true;
}

uint16_t etmPlusNextFrequency(uint16_t current, int16_t direction)
{
  uint8_t hour;
  if(!etmPlusSupported() || !etmPlusCurrentHour(&hour) ||
     !etmPlusLoad(bandIdx, hour) || !etmPlus->count || !direction) return 0;

  int32_t index = direction > 0 ? 0 : etmPlus->count - 1;
  if(direction > 0)
  {
    for(uint16_t i = 0; i < etmPlus->count; ++i)
      if(etmPlus->frequencies[i] > current)
      {
        index = i;
        break;
      }
  }
  else
  {
    for(int16_t i = etmPlus->count - 1; i >= 0; --i)
      if(etmPlus->frequencies[i] < current)
      {
        index = i;
        break;
      }
  }

  int32_t steps = direction > 0 ? direction : -(int32_t)direction;
  int32_t offset = (steps - 1) % etmPlus->count;
  if(direction < 0) offset = etmPlus->count - offset;
  index = (index + offset) % etmPlus->count;
  return etmPlus->frequencies[index];
}

uint16_t etmPlusFrequencyPosition(uint16_t current, uint16_t *total)
{
  uint8_t hour;
  if(!etmPlusSupported() || !etmPlusCurrentHour(&hour) || !etmPlusLoad(bandIdx, hour))
  {
    if(total) *total = 0;
    return 0;
  }

  uint16_t position = 0;
  for(uint16_t i = 0; i < etmPlus->count; ++i)
    if(etmPlus->frequencies[i] == current)
    {
      position = i + 1;
      break;
    }
  if(total) *total = etmPlus->count;
  return position;
}

static bool saveEtmPlus(const SavedEtmPlus &updated, uint8_t band, uint8_t hour)
{
  char key[16];
  etmPlusKey(key, band, hour);
  prefs.begin("etm-plus", false, STORAGE_PARTITION);
  bool saved = prefs.putBytes(key, &updated, sizeof(updated)) == sizeof(updated);
  prefs.end();
  if(saved)
  {
    *etmPlus = updated;
    loadedBand = band;
    loadedHour = hour;
  }
  return saved;
}

bool etmPlusScanning() { return activeScan != nullptr; }
uint16_t etmPlusScanFoundCount() { return scanFoundCount; }
uint8_t etmPlusScanListCount() { return activeScan ? recentCount : 0; }
uint8_t etmPlusScanHour() { return activeScanHour; }
uint16_t etmPlusScanFrequency(uint8_t index)
{
  return activeScan && index < recentCount ? recentFound[index] : 0;
}

static void scanProgress(uint16_t freq)
{
  currentFrequency = freq;
  drawScreen();
}

static bool scanShouldStop()
{
  if(consumeAbortPending()) scanAborted = true;
  return scanAborted;
}

static void rememberFrequency(SavedEtmPlus &found, uint16_t freq)
{
  uint16_t index = 0;
  while(index < found.count && found.frequencies[index] < freq) ++index;
  if((index < found.count && found.frequencies[index] == freq) || found.count == ETM_PLUS_LIMIT) return;
  for(uint16_t i = found.count; i > index; --i)
    found.frequencies[i] = found.frequencies[i - 1];
  found.frequencies[index] = freq;
  ++found.count;
  ++scanFoundCount;
  if(recentCount < ITEM_COUNT(recentFound))
    recentFound[recentCount++] = freq;
  else
  {
    for(uint8_t i = 1; i < recentCount; ++i)
      recentFound[i - 1] = recentFound[i];
    recentFound[recentCount - 1] = freq;
  }
}

EtmPlusScanResult etmPlusScan()
{
  if(!etmPlusSupported()) return EtmPlusScanResult::UNSUPPORTED;

  uint8_t hour;
  if(!etmPlusCurrentHour(&hour)) return EtmPlusScanResult::NO_CLOCK;
  if(!etmPlusLoad(bandIdx, hour)) return EtmPlusScanResult::NO_MEMORY;

  SavedEtmPlus *found = static_cast<SavedEtmPlus *>(ps_malloc(sizeof(*found)));
  if(!found) return EtmPlusScanResult::NO_MEMORY;
  *found = *etmPlus;
  found->count = 0;

  const uint8_t scanBand = bandIdx;
  const Band *band = getCurrentBand();
  const uint16_t originalFreq = currentFrequency;
  scanAborted = false;
  scanFoundCount = 0;
  recentCount = 0;
  activeScan = found;
  activeScanHour = hour;
  seekStop = false;
  clearStationInfo();
  muteOn(MUTE_TEMP, true);
  uint16_t scanMinimum = band->minimumFreq;
  uint16_t scanMaximum = band->maximumFreq;
  const bool broadcastOnly = shortwaveBroadcastOnly();
  if(broadcastOnly)
    shortwaveBroadcastRange(scanMinimum, 1, &scanMinimum, &scanMaximum);
  rx.setSeekAmLimits(scanMinimum, scanMaximum);
  rx.setFrequency(scanMinimum);
  scanProgress(scanMinimum);
  rx.getCurrentReceivedSignalQuality();
  if(rx.getCurrentRSSI() >= 10 && rx.getCurrentSNR() >= 3)
  {
    rememberFrequency(*found, scanMinimum);
    drawScreen();
  }

  uint16_t previous = scanMinimum;
  while(!scanShouldStop())
  {
    rx.seekStationProgress(scanProgress, scanShouldStop, 1);
    if(scanAborted) break;
    if(rx.getBandLimit())
    {
      if(!broadcastOnly ||
         !shortwaveBroadcastRange(scanMaximum + 1, 1, &scanMinimum, &scanMaximum)) break;
      rx.setSeekAmLimits(scanMinimum, scanMaximum);
      rx.setFrequency(scanMinimum);
      scanProgress(scanMinimum);
      previous = scanMinimum;
      rx.getCurrentReceivedSignalQuality();
      if(rx.getCurrentRSSI() >= 10 && rx.getCurrentSNR() >= 3)
      {
        rememberFrequency(*found, scanMinimum);
        drawScreen();
      }
      continue;
    }

    const uint16_t freq = rx.getFrequency();
    if(freq > band->maximumFreq) break;
    if(freq <= previous)
    {
      uint32_t next = (uint32_t)previous + getCurrentStep()->spacing;
      if(next > band->maximumFreq) break;
      rx.setFrequency(next);
      previous = next;
      continue;
    }
    previous = freq;
    if(!rx.getStatusValid()) continue;

    rx.getCurrentReceivedSignalQuality();
    if(rx.getCurrentRSSI() < 10 || rx.getCurrentSNR() < 3) continue;
    rememberFrequency(*found, freq);
    drawScreen();
    if(freq == band->maximumFreq) break;
  }

  activeScan = nullptr;
  rx.setSeekAmLimits(band->minimumFreq, band->maximumFreq);
  rx.setFrequency(originalFreq);
  currentFrequency = originalFreq;
  muteOn(MUTE_TEMP, false);
  clearStationInfo();
  identifyFrequency(currentFrequency);

  EtmPlusScanResult result = EtmPlusScanResult::CANCELLED;
  if(!scanAborted)
    result = saveEtmPlus(*found, scanBand, hour) ? EtmPlusScanResult::COMPLETED :
                                                   EtmPlusScanResult::SAVE_FAILED;
  free(found);
  return result;
}
