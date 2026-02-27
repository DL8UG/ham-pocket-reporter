
#ifndef DXCC_COUNTRY_TABLE_H
#define DXCC_COUNTRY_TABLE_H

#include <Arduino.h>

struct DxccCountry {
    const char* prefix;   // DXCC/ITU callsign prefix (startsWith match)
    const char* iso3;     // ISO 3166-1 alpha-3 of sovereign state (e.g., DEU, USA, GBR)
};

// NOTE:
//  * Territorial prefixes (e.g., CT3 Madeira, EA8 Canary) are mapped to the
//    sovereign state's ISO-3 code (PRT for Portugal, ESP for Spain, etc.).
//  * Matching strategy in your code should use startsWith() against 'prefix'.
//  * The full table is split across 5 include parts for readability.

const DxccCountry dxccCountryTable[] PROGMEM = {
  #include "dxcc_country_table_part1_europe.h"
  #include "dxcc_country_table_part2_north_america.h"
  #include "dxcc_country_table_part3_south_america.h"
  #include "dxcc_country_table_part4_asia.h"
  #include "dxcc_country_table_part5_africa_oceania.h"
};

const uint16_t dxccCountryTableSize = sizeof(dxccCountryTable) / sizeof(DxccCountry);

#endif // DXCC_COUNTRY_TABLE_H
