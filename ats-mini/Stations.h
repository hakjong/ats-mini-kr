#ifndef STATIONS_H
#define STATIONS_H

#include <stdint.h>

#define STATION_BACK         0
#define STATION_ADD_CURRENT  1
#define STATION_ATS_SCAN     2
#define STATION_CLEAR        3
#define STATION_ACTION_COUNT 4
#define STATION_GROUP_COUNT  3

enum StationGroup : uint8_t
{
  STATION_GROUP_FM,
  STATION_GROUP_MW,
  STATION_GROUP_SW,
};

enum class StationAddResult : uint8_t
{
  ADDED,
  ALREADY_SAVED,
  LIST_FULL,
  SAVE_FAILED,
};

enum class StationScanResult : uint8_t
{
  COMPLETED,
  CANCELLED,
  SAVE_FAILED,
  UNSUPPORTED,
};

void stationsLoad(uint8_t band);
void stationsSelectFrequency(uint16_t frequency);
StationScanResult stationsScan();
void stationsSelect(int16_t direction);
StationAddResult stationsAddCurrent();
bool stationsClear();
bool stationsDeleteSelected();
uint16_t stationsCount();
bool stationsHasFrequency(uint16_t frequency);
uint16_t stationsSelected();
uint16_t stationsFrequency(uint16_t index);
uint16_t stationsNextFrequency(uint16_t current, int16_t direction);
uint16_t stationsFrequencyPosition(uint16_t current, uint16_t *total);
bool stationsScanning();
uint16_t stationsScanFoundCount();
uint8_t stationsScanListCount();
uint16_t stationsScanFrequency(uint8_t index);
bool stationsReadGroup(uint8_t group, uint16_t *frequencies, uint16_t *count);
bool stationsValidateGroup(uint8_t group, const uint16_t *frequencies, uint16_t count);
bool stationsWriteGroup(uint8_t group, const uint16_t *frequencies, uint16_t count);

#endif // STATIONS_H
