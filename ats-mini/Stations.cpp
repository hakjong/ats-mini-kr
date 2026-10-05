#include "Common.h"
#include "Draw.h"
#include "Menu.h"
#include "Storage.h"
#include "Stations.h"
#include "Shortwave.h"
#include "Utils.h"
#include "KrFm.h"

#define STATION_LIMIT 256
#define STATION_VERSION 3

struct SavedStations
{
  uint8_t version;
  uint8_t group;
  uint16_t count;
  uint16_t reserved[2];
  uint16_t frequencies[STATION_LIMIT];
};

static SavedStations stations = {};
static uint8_t loadedGroup = 255;
static uint8_t selectedBand = 255;
static uint16_t selected = 0;
static bool scanAborted = false;
static const SavedStations *activeScan = nullptr;
static uint16_t scanFoundCount = 0;
static uint16_t recentFound[5] = {};
static uint8_t recentCount = 0;

static uint8_t stationGroup(uint8_t band)
{
  if(bands[band].bandType == FM_BAND_TYPE) return STATION_GROUP_FM;
  if(bands[band].bandType == MW_BAND_TYPE || bands[band].bandType == LW_BAND_TYPE)
    return STATION_GROUP_MW;
  return STATION_GROUP_SW;
}

static void stationKey(char *key, uint8_t group)
{
  snprintf(key, 16, "Group-%u", group);
}

static bool frequencyInGroup(uint16_t frequency, uint8_t group)
{
  for(int i = 0; i < getTotalBands(); ++i)
    if(stationGroup(i) == group && frequency >= bands[i].minimumFreq && frequency <= bands[i].maximumFreq)
      return true;
  return false;
}

static bool frequencyInCurrentBand(uint16_t frequency)
{
  const Band *band = getCurrentBand();
  return frequency >= band->minimumFreq && frequency <= band->maximumFreq;
}

static bool readGroup(SavedStations &saved, uint8_t group)
{
  if(group >= STATION_GROUP_COUNT) return false;

  char key[16];
  stationKey(key, group);
  saved = {};
  prefs.begin("stations", true, STORAGE_PARTITION);
  if(prefs.getBytesLength(key) == sizeof(saved))
    prefs.getBytes(key, &saved, sizeof(saved));
  prefs.end();

  if(saved.version != STATION_VERSION || saved.group != group || saved.count > STATION_LIMIT)
    return false;
  for(uint16_t i = 0; i < saved.count; ++i)
    if(!frequencyInGroup(saved.frequencies[i], group) ||
       (i && saved.frequencies[i] <= saved.frequencies[i - 1]))
      return false;
  return true;
}

void stationsLoad(uint8_t band)
{
  uint8_t group = stationGroup(band);
  if(selectedBand != band)
  {
    selectedBand = band;
    selected = 0;
  }
  if(loadedGroup == group && stations.group == group) return;

  loadedGroup = group;
  if(!readGroup(stations, group))
  {
    stations = {};
    stations.version = STATION_VERSION;
    stations.group = group;
  }
  krFmSetStations(stations.frequencies, group == STATION_GROUP_FM ? stations.count : 0);
}

uint16_t stationsCount()
{
  uint16_t count = 0;
  for(uint16_t i = 0; i < stations.count; ++i)
    if(frequencyInCurrentBand(stations.frequencies[i])) ++count;
  return count;
}

bool stationsHasFrequency(uint16_t frequency)
{
  stationsLoad(bandIdx);
  for(uint16_t i = 0; i < stations.count; ++i)
    if(stations.frequencies[i] == frequency) return true;
  return false;
}

uint16_t stationsSelected() { return selected; }
void stationsSelectFrequency(uint16_t frequency)
{
  selected = STATION_BACK;
  uint16_t visibleIndex = 0;
  for(uint16_t i = 0; i < stations.count; ++i)
    if(frequencyInCurrentBand(stations.frequencies[i]))
    {
      if(stations.frequencies[i] == frequency)
      {
        selected = visibleIndex + STATION_ACTION_COUNT;
        return;
      }
      ++visibleIndex;
    }
}

uint16_t stationsFrequency(uint16_t index)
{
  for(uint16_t i = 0; i < stations.count; ++i)
    if(frequencyInCurrentBand(stations.frequencies[i]) && !index--)
      return stations.frequencies[i];
  return 0;
}

static int32_t nextBandIndex(int32_t index, int8_t direction)
{
  for(uint16_t i = 0; i < stations.count; ++i)
  {
    index = (index + stations.count + direction) % stations.count;
    if(frequencyInCurrentBand(stations.frequencies[index])) return index;
  }
  return -1;
}

uint16_t stationsNextFrequency(uint16_t current, int16_t direction)
{
  stationsLoad(bandIdx);
  uint16_t count = stationsCount();
  if(!count || !direction) return 0;

  int32_t index = -1;
  if(direction > 0)
  {
    for(uint16_t i = 0; i < stations.count; ++i)
      if(frequencyInCurrentBand(stations.frequencies[i]) && stations.frequencies[i] > current)
      {
        index = i;
        break;
      }
  }
  else
  {
    for(int16_t i = stations.count - 1; i >= 0; --i)
      if(frequencyInCurrentBand(stations.frequencies[i]) && stations.frequencies[i] < current)
      {
        index = i;
        break;
      }
  }

  if(index < 0)
    index = nextBandIndex(direction > 0 ? -1 : stations.count, direction > 0 ? 1 : -1);
  int32_t steps = direction > 0 ? direction : -(int32_t)direction;
  for(int32_t i = 0; i < (steps - 1) % count; ++i)
    index = nextBandIndex(index, direction > 0 ? 1 : -1);
  return stations.frequencies[index];
}

uint16_t stationsFrequencyPosition(uint16_t current, uint16_t *total)
{
  stationsLoad(bandIdx);
  uint16_t position = 0;
  uint16_t visibleCount = 0;
  for(uint16_t i = 0; i < stations.count; ++i)
    if(frequencyInCurrentBand(stations.frequencies[i]))
    {
      ++visibleCount;
      if(stations.frequencies[i] == current) position = visibleCount;
    }
  if(total) *total = visibleCount;
  return position;
}

bool stationsReadGroup(uint8_t group, uint16_t *frequencies, uint16_t *count)
{
  if(!frequencies || !count) return false;
  SavedStations saved;
  if(!readGroup(saved, group))
  {
    *count = 0;
    return group < STATION_GROUP_COUNT;
  }
  memcpy(frequencies, saved.frequencies, saved.count * sizeof(saved.frequencies[0]));
  *count = saved.count;
  return true;
}

bool stationsValidateGroup(uint8_t group, const uint16_t *frequencies, uint16_t count)
{
  if(group >= STATION_GROUP_COUNT || count > STATION_LIMIT || (count && !frequencies)) return false;
  for(uint16_t i = 0; i < count; ++i)
    if(!frequencyInGroup(frequencies[i], group) || (i && frequencies[i] <= frequencies[i - 1]))
      return false;
  return true;
}

bool stationsWriteGroup(uint8_t group, const uint16_t *frequencies, uint16_t count)
{
  if(!stationsValidateGroup(group, frequencies, count)) return false;
  SavedStations updated = {};
  updated.version = STATION_VERSION;
  updated.group = group;
  updated.count = count;
  for(uint16_t i = 0; i < count; ++i)
    updated.frequencies[i] = frequencies[i];

  char key[16];
  stationKey(key, group);
  prefs.begin("stations", false, STORAGE_PARTITION);
  bool saved = prefs.putBytes(key, &updated, sizeof(updated)) == sizeof(updated);
  prefs.end();
  if(saved && loadedGroup == group)
  {
    stations = updated;
    if(group == STATION_GROUP_FM) krFmSetStations(stations.frequencies, stations.count);
  }
  return saved;
}

static bool saveStations(const SavedStations &updated)
{
  char key[16];
  stationKey(key, stationGroup(bandIdx));
  prefs.begin("stations", false, STORAGE_PARTITION);
  bool saved = prefs.putBytes(key, &updated, sizeof(updated)) == sizeof(updated);
  prefs.end();
  if(saved)
  {
    stations = updated;
    krFmSetStations(stations.frequencies, currentMode == FM ? stations.count : 0);
    clearStationInfo();
    identifyFrequency(currentFrequency);
  }
  return saved;
}

static void removeCurrentBand(SavedStations &updated)
{
  uint16_t kept = 0;
  const uint16_t oldCount = updated.count;
  for(uint16_t i = 0; i < oldCount; ++i)
    if(!frequencyInCurrentBand(updated.frequencies[i]))
      updated.frequencies[kept++] = updated.frequencies[i];
  for(uint16_t i = kept; i < oldCount; ++i) updated.frequencies[i] = 0;
  updated.count = kept;
}

bool stationsClear()
{
  stationsLoad(bandIdx);
  SavedStations cleared = stations;
  removeCurrentBand(cleared);
  if(!saveStations(cleared)) return false;
  if(currentMode == FM)
  {
    krFmSetManualRegion(KR_FM_AUTO);
    prefsRequestSave(SAVE_SETTINGS, true);
    clearStationInfo();
    identifyFrequency(currentFrequency);
  }
  selected = STATION_CLEAR; // Keep Clear selected after removing the list.
  return true;
}

StationAddResult stationsAddCurrent()
{
  stationsLoad(bandIdx);
  uint16_t index = 0;
  while(index < stations.count && stations.frequencies[index] < currentFrequency) ++index;
  if(index < stations.count && stations.frequencies[index] == currentFrequency)
    return StationAddResult::ALREADY_SAVED;
  if(stations.count == STATION_LIMIT) return StationAddResult::LIST_FULL;

  SavedStations updated = stations;
  for(uint16_t i = updated.count; i > index; --i)
    updated.frequencies[i] = updated.frequencies[i - 1];
  updated.frequencies[index] = currentFrequency;
  ++updated.count;
  return saveStations(updated) ? StationAddResult::ADDED : StationAddResult::SAVE_FAILED;
}

bool stationsDeleteSelected()
{
  stationsLoad(bandIdx);
  uint16_t visibleCount = stationsCount();
  if(selected < STATION_ACTION_COUNT || selected >= visibleCount + STATION_ACTION_COUNT) return false;
  SavedStations updated = stations;
  uint16_t visibleIndex = selected - STATION_ACTION_COUNT;
  uint16_t index = 0;
  while(index < updated.count)
  {
    if(frequencyInCurrentBand(updated.frequencies[index]))
    {
      if(!visibleIndex) break;
      --visibleIndex;
    }
    ++index;
  }
  if(index >= updated.count) return false;
  for(uint16_t i = index; i + 1 < updated.count; ++i)
    updated.frequencies[i] = updated.frequencies[i + 1];
  updated.frequencies[--updated.count] = 0;
  if(!saveStations(updated)) return false;
  if(selected >= stationsCount() + STATION_ACTION_COUNT) --selected;
  return true;
}

bool stationsScanning() { return activeScan != nullptr; }
uint16_t stationsScanFoundCount() { return scanFoundCount; }
uint8_t stationsScanListCount() { return activeScan ? recentCount : 0; }
uint16_t stationsScanFrequency(uint8_t index)
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

static void rememberStation(SavedStations &found, uint16_t freq)
{
  uint16_t index = 0;
  while(index < found.count && found.frequencies[index] < freq) ++index;
  if((index < found.count && found.frequencies[index] == freq) || found.count == STATION_LIMIT) return;
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

StationScanResult stationsScan()
{
  if(isSSB()) return StationScanResult::UNSUPPORTED; // The SI4732 cannot seek in SSB mode.

  stationsLoad(bandIdx);
  const Band *band = getCurrentBand();
  const uint16_t originalFreq = currentFrequency;
  SavedStations found = stations;
  removeCurrentBand(found);

  scanAborted = false;
  scanFoundCount = 0;
  recentCount = 0;
  activeScan = &found;
  seekStop = false;
  clearStationInfo();
  muteOn(MUTE_TEMP, true);
  uint16_t scanMinimum = band->minimumFreq;
  uint16_t scanMaximum = band->maximumFreq;
  const bool broadcastOnly = shortwaveBroadcastOnly();
  if(broadcastOnly)
    shortwaveBroadcastRange(scanMinimum, 1, &scanMinimum, &scanMaximum);
  if(broadcastOnly) rx.setSeekAmLimits(scanMinimum, scanMaximum);
  rx.setFrequency(scanMinimum);
  scanProgress(scanMinimum);
  rx.getCurrentReceivedSignalQuality();
  if(rx.getCurrentRSSI() >= (currentMode == FM ? 5 : 10) &&
     rx.getCurrentSNR() >= (currentMode == FM ? 2 : 3))
  {
    rememberStation(found, scanMinimum);
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
        rememberStation(found, scanMinimum);
        drawScreen();
      }
      continue;
    }

    const uint16_t freq = rx.getFrequency();
    if(freq > band->maximumFreq) break;
    if(freq <= previous)
    {
      // A seek may time out without moving. Advance before trying again.
      uint32_t next = (uint32_t)previous + getCurrentStep()->spacing;
      if(next > band->maximumFreq) break;
      rx.setFrequency(next);
      previous = next;
      continue;
    }
    previous = freq;
    if(!rx.getStatusValid()) continue; // Seek timed out; resume from here.

    rx.getCurrentReceivedSignalQuality();
    if(rx.getCurrentRSSI() < (currentMode == FM ? 5 : 10) ||
       rx.getCurrentSNR() < (currentMode == FM ? 2 : 3)) continue;
    rememberStation(found, freq);
    drawScreen();
    if(freq == band->maximumFreq) break;
  }

  activeScan = nullptr;
  if(currentMode != FM) rx.setSeekAmLimits(band->minimumFreq, band->maximumFreq);
  rx.setFrequency(originalFreq);
  currentFrequency = originalFreq;
  muteOn(MUTE_TEMP, false);
  clearStationInfo();
  identifyFrequency(currentFrequency);

  if(scanAborted) return StationScanResult::CANCELLED;
  if(!saveStations(found)) return StationScanResult::SAVE_FAILED;
  if(currentMode == FM)
  {
    krFmSetManualRegion(KR_FM_AUTO);
    prefsRequestSave(SAVE_SETTINGS, true);
    clearStationInfo();
    identifyFrequency(currentFrequency);
  }
  return StationScanResult::COMPLETED;
}

void stationsSelect(int16_t direction)
{
  stationsLoad(bandIdx);
  if(!direction) return;
  int16_t total = stationsCount() + STATION_ACTION_COUNT;
  selected = (selected + total + direction % total) % total;
  if(selected < STATION_ACTION_COUNT) return;
  updateFrequency(stationsFrequency(selected - STATION_ACTION_COUNT), false);
  clearStationInfo();
  identifyFrequency(currentFrequency);
  prefsRequestSave(SAVE_CUR_BAND);
}
