// ============================================================================
// Weather Helper Functions for HALO v1 ESPHome Configuration
// ============================================================================
// This header provides reusable C++ functions to eliminate code duplication
// in weather forecast display scripts. These functions are called hundreds
// of times, reducing the compiled binary size and improving maintainability.
//
// Memory Impact: Reduces ~3000 lines of duplicate code to ~200 lines
// Flash Savings: Estimated 50-80KB reduction in compiled binary size
// ============================================================================

#pragma once

#include <string>
#include <algorithm>
#include <cmath>

namespace weather_helpers {

// ============================================================================
// Timezone and DateTime Conversion
// ============================================================================

/**
 * Convert datetime string from forecast API to local time hour (0-23)
 * 
 * Handles multiple formats:
 *   - "2025-11-10T14:00:00Z" (UTC/Zulu time)
 *   - "2025-11-10T14:00:00-0400" (with numeric offset)
 *   - "2025-11-10T14:00:00" (no timezone, assumes UTC)
 * 
 * @param datetime_str The datetime string from weather API
 * @param local_tz_offset Local timezone offset in seconds (from ESPHome time component)
 * @return Local hour (0-23), or -1 if parsing fails
 */
inline int convert_to_local_hour(const std::string& datetime_str, int32_t local_tz_offset) {
    if (datetime_str.length() < 13) {
        return -1;  // Invalid format
    }
    
    // Extract hour from "YYYY-MM-DDTHH:MM:SS"
    std::string hour_str = datetime_str.substr(11, 2);
    int hour = std::stoi(hour_str);
    int source_offset_hours = 0;
    
    // Parse timezone offset from source datetime
    if (datetime_str.length() >= 20) {
        if (datetime_str[19] == 'Z') {
            // Zulu/UTC time
            source_offset_hours = 0;
        } else if (datetime_str.length() >= 24 && 
                   (datetime_str[19] == '+' || datetime_str[19] == '-')) {
            // Numeric offset (e.g., "-0400")
            source_offset_hours = std::stoi(datetime_str.substr(20, 2));
            if (datetime_str[19] == '-') {
                source_offset_hours = -source_offset_hours;
            }
        }
    }
    
    // Convert source to UTC, then to local
    int local_offset_hours = local_tz_offset / 3600;
    int utc_hour = hour - source_offset_hours;
    hour = utc_hour + local_offset_hours;
    
    // Handle day wraparound
    while (hour < 0) hour += 24;
    while (hour >= 24) hour -= 24;
    
    return hour;
}

/**
 * Format hour as HH:00 string
 */
inline std::string format_hour(int hour) {
    if (hour < 0 || hour > 23) {
        return "--:--";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:00", hour);
    return std::string(buf);
}

/**
 * Parse datetime string and extract month/day for forecast headers
 * Handles timezone conversion to show correct local date
 * 
 * @param datetime_str The datetime string from API
 * @param local_tz_offset Local timezone offset in seconds
 * @param month Output parameter for month (1-12)
 * @param day Output parameter for day (1-31)
 * @return true if parsing succeeded, false otherwise
 */
inline bool parse_forecast_date(const std::string& datetime_str, 
                                 int32_t local_tz_offset,
                                 int& month, 
                                 int& day) {
    if (datetime_str.length() < 19) {
        return false;
    }
    
    int year = std::stoi(datetime_str.substr(0, 4));
    month = std::stoi(datetime_str.substr(5, 2));
    day = std::stoi(datetime_str.substr(8, 2));
    int hour = std::stoi(datetime_str.substr(11, 2));
    
    // Parse timezone offset if present
    if (datetime_str.length() >= 25 && 
        (datetime_str[19] == '+' || datetime_str[19] == '-')) {
        int offset_hours = std::stoi(datetime_str.substr(20, 2));
        if (datetime_str[19] == '-') offset_hours = -offset_hours;
        
        int local_offset_hours = local_tz_offset / 3600;
        
        // Adjust hour for timezone difference
        hour += (local_offset_hours - offset_hours);
        
        // Handle day boundary crossing
        if (hour < 0) {
            day--;
            if (day < 1) {
                month--;
                if (month < 1) {
                    month = 12;
                    year--;
                }
                // Days in each month (non-leap year)
                int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
                    day = 29;  // Leap year February
                } else {
                    day = days_in_month[month-1];
                }
            }
        } else if (hour >= 24) {
            day++;
            int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            int max_days = days_in_month[month-1];
            if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
                max_days = 29;
            }
            if (day > max_days) {
                day = 1;
                month++;
                if (month > 12) {
                    month = 1;
                    year++;
                }
            }
        }
    }
    
    return true;
}

// ============================================================================
// Weather Icon Mapping
// ============================================================================

/**
 * Map weather condition string to Material Design Icon unicode glyph
 * 
 * @param condition Weather condition string (e.g., "sunny", "rainy", "cloudy")
 * @return Unicode string for Material Design weather icon
 */
inline std::string get_weather_icon(const std::string& condition) {
    if (condition.empty()) {
        return "\U000F14E4";  // question-mark-circle (unknown)
    }
    
    // Check for specific compound conditions first (order matters!)
    if (condition.find("lightning-rainy") != std::string::npos) {
        return "\U000F067E";  // lightning-rainy
    }
    if (condition.find("snowy-rainy") != std::string::npos) {
        return "\U000F067F";  // snowy-rainy
    }
    if (condition.find("clear-night") != std::string::npos) {
        return "\U000F0594";  // clear-night
    }
    if (condition.find("partlycloudy") != std::string::npos) {
        return "\U000F0595";  // partlycloudy
    }
    
    // Single conditions
    if (condition.find("lightning") != std::string::npos) return "\U000F0593";
    if (condition.find("snowy") != std::string::npos) return "\U000F0598";
    if (condition.find("rainy") != std::string::npos) return "\U000F0597";
    if (condition.find("pouring") != std::string::npos) return "\U000F0596";
    if (condition.find("hail") != std::string::npos) return "\U000F0592";
    if (condition.find("fog") != std::string::npos) return "\U000F0591";
    if (condition.find("windy-variant") != std::string::npos) return "\U000F059E";
    if (condition.find("windy") != std::string::npos) return "\U000F059D";
    if (condition.find("cloudy") != std::string::npos) return "\U000F0590";
    if (condition.find("sunny") != std::string::npos || 
        condition.find("clear") != std::string::npos) return "\U000F0599";
    if (condition.find("exceptional") != std::string::npos) return "\U000F0F2F";
    
    return "\U000F0599";  // default to sunny
}

/**
 * Get color for weather icon based on condition
 * Returns lv_color_t compatible hex value
 */
inline uint32_t get_weather_icon_color(const std::string& condition) {
    if (condition.find("clear-night") != std::string::npos) return 0x6A5ACD;  // Slate Blue
    if (condition.find("sunny") != std::string::npos || 
        condition.find("clear") != std::string::npos) return 0xFFD700;  // Gold
    if (condition.find("snowy-rainy") != std::string::npos) return 0xAFEEEE;  // Light Cyan
    if (condition.find("lightning-rainy") != std::string::npos || 
        condition.find("lightning") != std::string::npos) return 0xFF1493;  // Deep Pink
    if (condition.find("partlycloudy") != std::string::npos) return 0x87CEFA;  // Light Sky Blue
    if (condition.find("cloudy") != std::string::npos) return 0xA9A9A9;  // Dark Gray
    if (condition.find("rainy") != std::string::npos || 
        condition.find("pouring") != std::string::npos) return 0x4169E1;  // Royal Blue
    if (condition.find("snowy") != std::string::npos) return 0xDCDCDC;  // Gainsboro
    if (condition.find("fog") != std::string::npos) return 0xB0C4DE;  // Light Steel Blue
    if (condition.find("windy") != std::string::npos) return 0x90EE90;  // Light Green
    
    return 0xFFFFFF;  // White (default)
}

/**
 * Format condition string to human-readable text
 * Converts "clear-night" → "Clear Night", "partlycloudy" → "Partly Cloudy", etc.
 */
inline std::string format_condition_text(const std::string& condition) {
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
    if (condition == "windy-variant") return "Very Windy";
    
    // Capitalize first letter if not matched
    std::string result = condition;
    if (!result.empty()) {
        result[0] = toupper(result[0]);
    }
    return result;
}

// ============================================================================
// Wind Direction Conversion
// ============================================================================

/**
 * Convert wind bearing (degrees) to cardinal direction abbreviation
 * 
 * @param bearing Wind direction in degrees (0-360)
 * @return Cardinal direction string (N, NE, E, SE, S, SW, W, NW)
 */
inline std::string bearing_to_direction(float bearing) {
    if (std::isnan(bearing)) {
        return "--";
    }
    
    if (bearing >= 337.5 || bearing < 22.5) return "N";
    if (bearing >= 22.5 && bearing < 67.5) return "NE";
    if (bearing >= 67.5 && bearing < 112.5) return "E";
    if (bearing >= 112.5 && bearing < 157.5) return "SE";
    if (bearing >= 157.5 && bearing < 202.5) return "S";
    if (bearing >= 202.5 && bearing < 247.5) return "SW";
    if (bearing >= 247.5 && bearing < 292.5) return "W";
    return "NW";
}

// ============================================================================
// Temperature and Value Formatting
// ============================================================================

/**
 * Format temperature with degree symbol
 * Handles NaN values gracefully
 */
inline std::string format_temp(float temp, const char* default_str = "--°") {
    if (std::isnan(temp)) {
        return std::string(default_str);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f°", temp);
    return std::string(buf);
}

/**
 * Format temperature with one decimal place
 */
inline std::string format_temp_precise(float temp, const char* default_str = "--.-°") {
    if (std::isnan(temp)) {
        return std::string(default_str);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f°", temp);
    return std::string(buf);
}

/**
 * Format percentage value
 */
inline std::string format_percent(float value, const char* default_str = "--%") {
    if (std::isnan(value)) {
        return std::string(default_str);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", value);
    return std::string(buf);
}

/**
 * Format precipitation amount in inches
 */
inline std::string format_precipitation(float value, const char* default_str = "--\"") {
    if (std::isnan(value)) {
        return std::string(default_str);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f\"", value);
    return std::string(buf);
}

/**
 * Format generic numeric value (pressure, UV index, etc.)
 */
inline std::string format_numeric(float value, const char* default_str = "--") {
    if (std::isnan(value)) {
        return std::string(default_str);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", value);
    return std::string(buf);
}

/**
 * Format high/low temperature pair with colors
 * Returns formatted string: "#FF0000 H:XX°# #00DFFF L:XX°#"
 */
inline std::string format_hi_lo_temps(float high, float low) {
    if (std::isnan(high) || std::isnan(low)) {
        return "H:--° L:--°";
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "#FF0000 H:%.0f°# #00DFFF L:%.0f°#", high, low);
    return std::string(buf);
}

}  // namespace weather_helpers
