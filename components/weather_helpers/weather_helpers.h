// ============================================================================
// Weather Helper Functions for HALO v1 ESPHome Configuration
// ============================================================================
// Shared formatting and parsing for the weather pages, so the LVGL lambdas
// call one implementation instead of each carrying its own copy.
//
// Every function returns std::string because LVGL `text:` lambdas expect one.
// The strings are short enough for the small-string optimisation, so they do
// not touch the heap. The one exception is format_hi_lo_temps(), whose
// recolour markup is longer than 15 bytes.
//
// Timestamps are parsed by hand, not with std::stoi: this build has C++
// exceptions disabled, so std::stoi aborts the firmware on a malformed string.
//
// Local time comes from localtime_r(). ESPHome's time component replaces it
// with its own POSIX-TZ implementation, so each timestamp gets the DST rule for
// its own date. A single fixed offset (e.g. ESPTime::timezone_offset(), which
// is today's) is an hour out for timestamps on the other side of a DST change.
// That matters for daily forecasts, which integrations commonly send as local
// midnight in UTC ("2026-09-26T04:00:00Z" for US Eastern).
// ============================================================================

#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace weather_helpers {

// ============================================================================
// Timestamp parsing (internal)
// ============================================================================

namespace detail {

// Parse `count` ASCII digits starting at `pos`. Returns -1 if the string is too
// short or any character is not a digit.
inline int parse_digits(const std::string &s, size_t pos, size_t count) {
  if (pos + count > s.length()) {
    return -1;
  }
  int value = 0;
  for (size_t i = pos; i < pos + count; i++) {
    char c = s[i];
    if (c < '0' || c > '9') {
      return -1;
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

// Days since 1970-01-01 for a proleptic Gregorian date, and the inverse
// (Howard Hinnant's days_from_civil / civil_from_days). Handles month lengths
// and leap years, so callers never roll dates over by hand.
inline int32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

inline void civil_from_days(int32_t z, int &y, int &m, int &d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe) + era * 400 + (m <= 2);
}

// Zone designator of an ISO-8601 timestamp, in minutes east of UTC.
// Accepts "Z", "+HH:MM", "+HHMM" and "+HH", after optional fractional seconds.
// A timestamp with no designator is treated as UTC.
inline bool parse_zone_minutes(const std::string &s, int &minutes) {
  minutes = 0;
  size_t i = 19;
  if (i < s.length() && s[i] == '.') {
    i++;
    while (i < s.length() && s[i] >= '0' && s[i] <= '9') {
      i++;
    }
  }
  if (i >= s.length() || s[i] == 'Z') {
    return true;
  }
  if (s[i] != '+' && s[i] != '-') {
    return false;
  }
  int hh = parse_digits(s, i + 1, 2);
  if (hh < 0) {
    return false;
  }
  int mm = 0;
  size_t m = i + 3;
  if (m < s.length() && s[m] == ':') {
    m++;
  }
  if (m < s.length()) {
    mm = parse_digits(s, m, 2);
    if (mm < 0) {
      return false;
    }
  }
  minutes = hh * 60 + mm;
  if (s[i] == '-') {
    minutes = -minutes;
  }
  return true;
}

// Parse "YYYY-MM-DD[T ]HH[:MM[:SS[.fff]]][zone]" to minutes since the epoch, UTC.
inline bool parse_utc_minutes(const std::string &s, int32_t &utc_minutes) {
  if (s.length() < 13 || (s[10] != 'T' && s[10] != ' ')) {
    return false;
  }
  int y = parse_digits(s, 0, 4);
  int mo = parse_digits(s, 5, 2);
  int d = parse_digits(s, 8, 2);
  int h = parse_digits(s, 11, 2);
  int mi = s.length() >= 16 ? parse_digits(s, 14, 2) : 0;
  if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) {
    return false;
  }
  int zone_minutes = 0;
  if (s.length() >= 19 && !parse_zone_minutes(s, zone_minutes)) {
    return false;
  }
  utc_minutes = days_from_civil(y, mo, d) * 1440 + h * 60 + mi - zone_minutes;
  return true;
}

// Local calendar date and hour, using the device time zone via localtime_r().
inline bool to_local(const std::string &s, int &year, int &month, int &day, int &hour) {
  int32_t minutes;
  if (!parse_utc_minutes(s, minutes)) {
    return false;
  }
  time_t epoch = static_cast<time_t>(minutes) * 60;
  struct tm local_tm;
  if (localtime_r(&epoch, &local_tm) == nullptr) {
    return false;
  }
  year = local_tm.tm_year + 1900;
  month = local_tm.tm_mon + 1;
  day = local_tm.tm_mday;
  hour = local_tm.tm_hour;
  return true;
}

}  // namespace detail

// ============================================================================
// Timezone and DateTime Conversion
// ============================================================================

/**
 * Local hour (0-23) of a forecast datetime string, in the device time zone.
 *
 * Handles "Z", "+HH:MM", "+HHMM" and no zone (treated as UTC), including
 * half-hour zones.
 *
 * @param datetime_str The datetime string from the weather API
 * @return Local hour (0-23), or -1 if parsing fails
 */
inline int convert_to_local_hour(const std::string &datetime_str) {
  int year, month, day, hour;
  if (!detail::to_local(datetime_str, year, month, day, hour)) {
    return -1;
  }
  return hour;
}

/**
 * Format hour as an HH:00 string, or "--:--" when out of range.
 */
inline std::string format_hour(int hour) {
  char buf[8];
  if (hour < 0 || hour > 23) {
    snprintf(buf, sizeof(buf), "--:--");
  } else {
    snprintf(buf, sizeof(buf), "%02d:00", hour);
  }
  return std::string(buf);
}

/**
 * Local month and day of a forecast datetime string, in the device time zone.
 *
 * @param datetime_str The datetime string from the weather API
 * @param month Output: month (1-12)
 * @param day Output: day (1-31)
 * @return true if parsing succeeded; month and day are untouched otherwise
 */
inline bool parse_forecast_date(const std::string &datetime_str, int &month, int &day) {
  int y, mo, d, h;
  if (!detail::to_local(datetime_str, y, mo, d, h)) {
    return false;
  }
  month = mo;
  day = d;
  return true;
}

// ============================================================================
// Weather Icon Mapping
// ============================================================================
// Glyphs must be listed in the weather icon fonts in
// packages/system/fonts_colors.yaml. pouring and hail reuse the rainy and
// snowy-rainy glyphs so the font does not grow.

/**
 * Map a Home Assistant weather condition to a Material Design Icons glyph.
 *
 * @param condition Weather condition string (e.g., "sunny", "rainy", "cloudy")
 * @return UTF-8 glyph; weather-sunny-off for unknown or empty conditions
 */
inline std::string get_weather_icon(const std::string &condition) {
  if (condition.empty()) {
    return "\U000F14E4";  // weather-sunny-off (unknown)
  }

  // Compound conditions first: they contain the single-condition names.
  if (condition.find("lightning-rainy") != std::string::npos) return "\U000F067E";
  if (condition.find("snowy-rainy") != std::string::npos) return "\U000F067F";
  if (condition.find("clear-night") != std::string::npos) return "\U000F0594";
  if (condition.find("partlycloudy") != std::string::npos) return "\U000F0595";

  // Single conditions
  if (condition.find("lightning") != std::string::npos) return "\U000F0593";
  if (condition.find("snowy") != std::string::npos) return "\U000F0598";
  if (condition.find("hail") != std::string::npos) return "\U000F067F";     // snowy-rainy glyph
  if (condition.find("pouring") != std::string::npos) return "\U000F0597";  // rainy glyph
  if (condition.find("rainy") != std::string::npos) return "\U000F0597";
  if (condition.find("fog") != std::string::npos) return "\U000F0591";
  if (condition.find("windy") != std::string::npos) return "\U000F059D";
  if (condition.find("cloudy") != std::string::npos) return "\U000F0590";
  if (condition.find("sunny") != std::string::npos || condition.find("clear") != std::string::npos) {
    return "\U000F0599";
  }

  return "\U000F14E4";  // weather-sunny-off (unknown, including "exceptional")
}

/**
 * Colour for a weather icon, as a hex value for lv_color_hex().
 */
inline uint32_t get_weather_icon_color(const std::string &condition) {
  if (condition.find("clear-night") != std::string::npos) return 0x6A5ACD;  // Slate Blue
  if (condition.find("sunny") != std::string::npos || condition.find("clear") != std::string::npos) {
    return 0xFFD700;  // Gold
  }
  if (condition.find("snowy-rainy") != std::string::npos) return 0xAFEEEE;  // Light Cyan
  if (condition.find("hail") != std::string::npos) return 0xAFEEEE;         // Light Cyan
  if (condition.find("lightning") != std::string::npos) return 0xFF1493;    // Deep Pink
  if (condition.find("partlycloudy") != std::string::npos) return 0x87CEFA; // Light Sky Blue
  if (condition.find("cloudy") != std::string::npos) return 0xA9A9A9;       // Dark Gray
  if (condition.find("rainy") != std::string::npos) return 0x4169E1;        // Royal Blue
  if (condition.find("pouring") != std::string::npos) return 0x4169E1;      // Royal Blue
  if (condition.find("snowy") != std::string::npos) return 0xDCDCDC;        // Gainsboro
  if (condition.find("fog") != std::string::npos) return 0xB0C4DE;          // Light Steel Blue
  if (condition.find("windy") != std::string::npos) return 0x90EE90;        // Light Green

  return 0xFFFFFF;  // White (default)
}

/**
 * Human-readable text for a Home Assistant weather condition.
 * Converts "clear-night" → "Clear Night", "partlycloudy" → "Partly Cloudy", etc.
 * Unknown conditions are returned with the first letter capitalised.
 */
inline std::string format_condition_text(const std::string &condition) {
  if (condition.empty()) return "Unknown";

  if (condition == "clear-night") return "Clear Night";
  if (condition == "cloudy") return "Cloudy";
  if (condition == "exceptional") return "Exceptional";
  if (condition == "fog") return "Fog";
  if (condition == "hail") return "Hail";
  if (condition == "lightning") return "Lightning";
  if (condition == "lightning-rainy") return "Lightning & Rain";
  if (condition == "partlycloudy") return "Partly Cloudy";
  if (condition == "pouring") return "Pouring";
  if (condition == "rainy") return "Rainy";
  if (condition == "snowy") return "Snowy";
  if (condition == "snowy-rainy") return "Sleet";
  if (condition == "sunny") return "Sunny";
  if (condition == "windy") return "Windy";
  if (condition == "windy-variant") return "Windy";

  std::string result = condition;
  result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
  return result;
}

// ============================================================================
// Wind Direction Conversion
// ============================================================================

/**
 * Convert wind bearing (degrees) to a cardinal direction abbreviation.
 *
 * @param bearing Wind direction in degrees (0-360)
 * @return N, NE, E, SE, S, SW, W, NW, or "--" if NaN
 */
inline std::string bearing_to_direction(float bearing) {
  if (std::isnan(bearing)) {
    return "--";
  }

  if (bearing >= 337.5 || bearing < 22.5) return "N";
  if (bearing < 67.5) return "NE";
  if (bearing < 112.5) return "E";
  if (bearing < 157.5) return "SE";
  if (bearing < 202.5) return "S";
  if (bearing < 247.5) return "SW";
  if (bearing < 292.5) return "W";
  return "NW";
}

// ============================================================================
// Temperature and Value Formatting
// ============================================================================
// Each returns `default_str` when the value is NaN.

/** Temperature rounded to a whole degree, e.g. "72°". */
inline std::string format_temp(float temp, const char *default_str = "--°") {
  char buf[16];
  if (std::isnan(temp)) {
    snprintf(buf, sizeof(buf), "%s", default_str);
  } else {
    snprintf(buf, sizeof(buf), "%.0f°", temp);
  }
  return std::string(buf);
}

/** Temperature with one decimal place, e.g. "72.4°". */
inline std::string format_temp_precise(float temp, const char *default_str = "--.-°") {
  char buf[16];
  if (std::isnan(temp)) {
    snprintf(buf, sizeof(buf), "%s", default_str);
  } else {
    snprintf(buf, sizeof(buf), "%.1f°", temp);
  }
  return std::string(buf);
}

/** Percentage rounded to a whole number, e.g. "40%". */
inline std::string format_percent(float value, const char *default_str = "--%") {
  char buf[16];
  if (std::isnan(value)) {
    snprintf(buf, sizeof(buf), "%s", default_str);
  } else {
    snprintf(buf, sizeof(buf), "%.0f%%", value);
  }
  return std::string(buf);
}

/** Precipitation in inches, e.g. "0.25\"". */
inline std::string format_precipitation(float value, const char *default_str = "--\"") {
  char buf[16];
  if (std::isnan(value)) {
    snprintf(buf, sizeof(buf), "%s", default_str);
  } else {
    snprintf(buf, sizeof(buf), "%.2f\"", value);
  }
  return std::string(buf);
}

/** Generic whole-number value (pressure, UV index, etc.). */
inline std::string format_numeric(float value, const char *default_str = "--") {
  char buf[16];
  if (std::isnan(value)) {
    snprintf(buf, sizeof(buf), "%s", default_str);
  } else {
    snprintf(buf, sizeof(buf), "%.0f", value);
  }
  return std::string(buf);
}

/**
 * High/low temperature pair with LVGL recolour markup:
 * "#FF0000 H:XX°# #00DFFF L:XX°#", or "H:--° L:--°" if either is NaN.
 */
inline std::string format_hi_lo_temps(float high, float low) {
  char buf[48];
  if (std::isnan(high) || std::isnan(low)) {
    snprintf(buf, sizeof(buf), "H:--° L:--°");
  } else {
    snprintf(buf, sizeof(buf), "#FF0000 H:%.0f°# #00DFFF L:%.0f°#", high, low);
  }
  return std::string(buf);
}

// ============================================================================
// Hourly Summary Update Helpers
// ============================================================================
// Thin wrappers so the hourly summary lambdas read uniformly.

/** Local "HH:00" for a forecast datetime string, or "--:--" if it cannot be parsed. */
inline std::string format_hourly_summary_time(const std::string &datetime_str) {
  return format_hour(convert_to_local_hour(datetime_str));
}

/** Weather icon glyph for the hourly summary. */
inline std::string format_hourly_summary_icon(const std::string &condition) { return get_weather_icon(condition); }

/** Temperature for the hourly summary ("XX°" or "--°"). */
inline std::string format_hourly_summary_temp(float temp) { return format_temp(temp); }

/** Precipitation probability for the hourly summary ("XX%" or "--%"). */
inline std::string format_hourly_summary_precip(float precip_prob) { return format_percent(precip_prob); }

}  // namespace weather_helpers
