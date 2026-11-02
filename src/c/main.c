#include <pebble.h>

// ==============================================================================
// GLOBAL VARIABLES & STATE TRACKING
// ==============================================================================

// Core UI elements
static Window *s_main_window;       // The primary window for the app
static Layer *s_canvas_layer;       // The custom drawing layer for the grid

// Data arrays sized for a full 24-hour forecast
static char s_forecast_data[48][4][10]; // Stores text data: [Hour][Data Type][String Value]
static int s_uv_values[48];             // Stores the raw UV index integers
static int s_is_night[48];              // Boolean flag (1 or 0) for day/night icon selection
static int s_conditions[48];            // OpenWeather condition ID codes (e.g., 500 = light rain)
static GBitmap *s_icon_bitmaps[48];     // Pointers to the loaded image assets in memory
static char s_pending_data[48][4][10];
static int s_pending_uv[48], s_pending_night[48], s_pending_conditions[48], s_pending_gust[48];
static int s_gust[48], s_forecast_count = 0, s_pending_count = 0;
static int s_pending_wind_unit = 0;
static bool s_pending_metric = false, s_pending_gust_enabled = false, s_gust_enabled = false;
static time_t s_forecast_timestamp = 0, s_pending_timestamp = 0;
static bool s_cached = false, s_pending_cached = false, s_pending_first = false;
static int s_pending_next_part = 0;
static int s_selected_hour = 0;
static char s_pending_location[32] = "";
static char s_status[48] = "";
static AppTimer *s_status_timer = NULL;
static time_t s_sunrise = 0, s_sunset = 0;
static time_t s_pending_sunrise = 0, s_pending_sunset = 0;
static bool s_last_auto_dark = false;
static int s_color_mode = 0; // 0=auto, 1=day, 2=night
static int s_pending_color_mode = 0;
static int s_wind_unit = 0; // 0=mph, 1=km/h, 2=m/s, 3=knots
static bool s_is_metric = false;        // Tracks user preference for Celsius/kmh vs Fahrenheit/mph

// Navigation and Layout State
static int s_scroll_offset = 0;
static bool s_has_forecast = false; // True after receiving forecast data

// Temporary Notification Banner State
static char s_location_name[32] = "Retrieving weather..."; // Holds the city name or loading text
static bool s_show_banner = true;                          // Toggles banner visibility
static AppTimer *s_banner_timer = NULL;                    // Timer handle for auto-dismissing the banner

// ==============================================================================
// MEMORY MANAGEMENT & ASSET LOADING
// ==============================================================================

// Loads only the icons currently visible on the screen to prevent RAM overflow
static void invalidate_icon(int idx) {
  if (s_icon_bitmaps[idx]) {
    gbitmap_destroy(s_icon_bitmaps[idx]);
    s_icon_bitmaps[idx] = NULL;
  }
}
static void update_icons_for_view() {
  for(int i = 0; i < 48; i++) {
    // Check if the index falls within the currently visible 6-hour window
    if (i >= s_scroll_offset && i < s_scroll_offset + (s_show_banner ? 5 : 6)) {
      
      // If the icon isn't loaded yet, and we have valid condition data
      if (!s_icon_bitmaps[i] && s_conditions[i] != 0) {
        bool night = (s_is_night[i] == 1);
        uint32_t res_id;
        int cid = s_conditions[i];

        // Map OpenWeather IDs to Pebble Resource IDs (Highest priority conditions checked first)
        if (cid == 800) {
          res_id = night ? RESOURCE_ID_ICON_CLEAR_NIGHT : RESOURCE_ID_ICON_CLEAR_DAY;
        } else if (cid == 801) {
          res_id = night ? RESOURCE_ID_ICON_CLOUDY_NIGHT : RESOURCE_ID_ICON_CLOUDY_SUN;
        } else if (cid >= 200 && cid <= 232) { 
          res_id = RESOURCE_ID_ICON_THUNDER_SHOWER;
        } else if (cid >= 300 && cid <= 321) { 
          res_id = night ? RESOURCE_ID_ICON_SHOWER_NIGHT : RESOURCE_ID_ICON_SHOWER_DAY;
        } else if (cid >= 500 && cid <= 501) { 
          res_id = RESOURCE_ID_ICON_LIGHT_RAIN;
        } else if (cid >= 502 && cid <= 504) { 
          res_id = RESOURCE_ID_ICON_HEAVY_RAIN;
        } else if (cid == 511) { 
          res_id = RESOURCE_ID_ICON_SLEET;
        } else if (cid >= 520 && cid <= 531) { 
          res_id = RESOURCE_ID_ICON_RAIN_STORM;
        } else if (cid == 600) { 
          res_id = RESOURCE_ID_ICON_LIGHT_SNOW;
        } else if (cid == 601) { 
          res_id = RESOURCE_ID_ICON_SNOW;
        } else if (cid == 602) { 
          res_id = RESOURCE_ID_ICON_HEAVY_SNOW;
        } else if (cid >= 620 && cid <= 622) { 
          res_id = night ? RESOURCE_ID_ICON_SNOW_SHOWER_NIGHT : RESOURCE_ID_ICON_SNOW_SHOWER_DAY;
        } else if (cid == 701 || cid == 741) { 
          res_id = RESOURCE_ID_ICON_FOG;
        } else if (cid == 711 || cid == 721) { 
          res_id = RESOURCE_ID_ICON_HAZE;
        } else if (cid == 731 || cid == 761) { 
          res_id = RESOURCE_ID_ICON_DUST;
        } else if (cid == 751) { 
          res_id = RESOURCE_ID_ICON_SANDSTORM;
        } else if (cid >= 802 && cid <= 804) { 
          res_id = RESOURCE_ID_ICON_CLOUDY; // Catch-all for heavy clouds
        } else {
          res_id = RESOURCE_ID_ICON_CLOUDY; // Ultimate fallback
        }

        // Allocate memory and create the bitmap
        s_icon_bitmaps[i] = gbitmap_create_with_resource(res_id);
      }
    } else {
      // If the row is off-screen, destroy the bitmap to free up RAM
      if (s_icon_bitmaps[i]) {
        gbitmap_destroy(s_icon_bitmaps[i]);
        s_icon_bitmaps[i] = NULL;
      }
    }
  }
}

// ==============================================================================
// EVENT HANDLERS & NAVIGATION
// ==============================================================================

// Fired when the 3-second banner timer expires
static void status_timer_callback(void *data) {
  s_status_timer = NULL;
  s_status[0] = '\0';
  layer_mark_dirty(s_canvas_layer);
}
static void show_status(const char *message) {
  snprintf(s_status, sizeof(s_status), "%s", message);
  if (s_status_timer) app_timer_cancel(s_status_timer);
  s_status_timer = app_timer_register(4000, status_timer_callback, NULL);
  layer_mark_dirty(s_canvas_layer);
}
static void banner_timer_callback(void *data) {
  s_show_banner = false;            // Hide the banner
  update_icons_for_view();
  s_banner_timer = NULL;            // Clear the timer reference
  layer_mark_dirty(s_canvas_layer); // Force a full UI redraw
}

// Handles scrolling up through the timeline
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scroll_offset > 0) {
    s_scroll_offset = s_scroll_offset > 2 ? s_scroll_offset - 2 : 0;
    update_icons_for_view();        // Reload memory assets for the new view
    layer_mark_dirty(s_canvas_layer); 
  }
}

// Handles scrolling down through the timeline
static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scroll_offset < s_forecast_count - 6) {
    s_scroll_offset = s_scroll_offset + 2 > s_forecast_count - 6 ? s_forecast_count - 6 : s_scroll_offset + 2;
    update_icons_for_view();        // Reload memory assets for the new view
    layer_mark_dirty(s_canvas_layer);
  }
}

// Binds hardware buttons to click functions
static void refresh_click_handler(ClickRecognizerRef recognizer, void *context) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK && out) {
    dict_write_uint8(out, MESSAGE_KEY_REFRESH_REQUEST, 1);
    dict_write_end(out);
    app_message_outbox_send();
    snprintf(s_location_name, sizeof(s_location_name), "Refreshing weather...");
    s_show_banner = true;
    update_icons_for_view();
    if (s_banner_timer) { app_timer_cancel(s_banner_timer); s_banner_timer = NULL; }
    layer_mark_dirty(s_canvas_layer);
  }
}
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_gust_enabled && s_has_forecast) {
    int count = s_forecast_count - s_scroll_offset;
    if (count > 6) count = 6;
    if (count < 1) return;
    int idx = s_scroll_offset + s_selected_hour % count;
    char message[48];
    const char *unit = s_wind_unit == 1 ? "km/h" : s_wind_unit == 2 ? "m/s" : s_wind_unit == 3 ? "kt" : "mph";
    snprintf(message, sizeof(message), "%s: gust %d %s", s_forecast_data[idx][0], s_gust[idx], unit);
    s_selected_hour = (s_selected_hour + 1) % count;
    show_status(message);
  } else if (s_has_forecast && s_forecast_timestamp) {
    int age = (int)((time(NULL) - s_forecast_timestamp) / 60);
    if (age < 0) age = 0;
    char message[48];
    snprintf(message, sizeof(message), "%s%d min old", s_cached ? "Cached: " : "Updated: ", age);
    show_status(message);
  } else {
    refresh_click_handler(recognizer, context);
  }
}
static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, refresh_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

// ==============================================================================
// DRAWING ENGINE (CANVAS LAYER)
// ==============================================================================

// The primary render loop called whenever the screen needs to update
static bool is_dark_mode(void) {
  if (s_color_mode == 1) return false;
  if (s_color_mode == 2) return true;
  time_t now = time(NULL);
  if (s_sunrise > 0 && s_sunset > s_sunrise) {
    // OpenWeather's sunrise/sunset describe the forecast location.
    // Shift yesterday/tomorrow's boundaries by 24h if needed.
    const time_t day = 86400;
    time_t sunrise = s_sunrise, sunset = s_sunset;
    if (now >= sunset) {
      time_t days = (now - sunset) / day;
      sunrise += days * day;
      sunset += days * day;
      if (now >= sunset) { sunrise += day; sunset += day; }
    } else if (now < sunrise) {
      time_t days = (sunrise - now + day - 1) / day;
      sunrise -= days * day;
      sunset -= days * day;
    }
    return !(now >= sunrise && now < sunset);
  }
  // Polar locations or missing astronomical data: fall back to local clock.
  struct tm *local = localtime(&now);
  return local && (local->tm_hour < 7 || local->tm_hour >= 19);
}
static void auto_theme_tick(struct tm *tick_time, TimeUnits units_changed) {
  if (s_color_mode != 0) return;
  bool dark = is_dark_mode();
  if (dark != s_last_auto_dark) {
    s_last_auto_dark = dark;
    layer_mark_dirty(s_canvas_layer);
  }
}
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer); // Get screen dimensions

  // Automatic appearance follows the watch local clock, not the forecast location.
  bool dark = is_dark_mode();
  GColor foreground = dark ? GColorWhite : GColorBlack;
  GColor background = dark ? GColorBlack : GColorWhite;
  GColor header_color = dark ? GColorDarkGray : GColorLightGray;
  GColor separator_color = dark ? GColorDarkGray : GColorLightGray;
  // 1. Draw appearance-dependent background
  graphics_context_set_fill_color(ctx, background);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // 2. Define headers and column spacing
  char *temp_label = s_is_metric ? "°C" : "°F";
  char *wind_label = s_wind_unit == 1 ? "km/h" :
                     s_wind_unit == 2 ? "m/s" :
                     s_wind_unit == 3 ? "kt" : "MPH"; 
  char *headers[] = {"Time", "Cond", temp_label, wind_label, "Rain"};
  int x_pos[] = {5, 42, 85, 122, 166}; // X-coordinates for each column

  // 3. Draw Header Background Bar
  graphics_context_set_fill_color(ctx, header_color);
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, 24), 0, GCornerNone);

  // 4. Draw Header Text
  graphics_context_set_text_color(ctx, foreground);
  for(int i = 0; i < 5; i++) {
    graphics_draw_text(ctx, headers[i], fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), 
                       GRect(x_pos[i], 0, i == 3 ? 38 : 42, 24), GTextOverflowModeTrailingEllipsis, i == 3 ? GTextAlignmentCenter : GTextAlignmentLeft, NULL);
  }

  // Determine how many rows to draw based on banner visibility (to prevent overlapping)
  int visible_rows = s_show_banner ? 5 : 6;
  if (visible_rows > s_forecast_count - s_scroll_offset) visible_rows = s_forecast_count - s_scroll_offset;
  if (visible_rows < 1) visible_rows = 1;
  int bottom_margin = s_show_banner ? 20 : 0; // Reserves 20px at the bottom if banner is active
  const int grid_height = bounds.size.h - 24 - bottom_margin;
  const int row_height = grid_height / visible_rows;

  // 5. Draw the Data Grid
  for(int i = 0; i < (s_has_forecast ? visible_rows : 0); i++) {
    int idx = i + s_scroll_offset;    // The actual data array index based on scroll position
    int y = 24 + (i * grid_height) / visible_rows; // Distribute rows evenly to the bottom
    
    // Feature: Highlight the "Current Hour" (Row 0) with a blue indicator on the left
    if (idx == 0 && !s_show_banner) {
      graphics_context_set_fill_color(ctx, GColorPictonBlue);
      graphics_fill_rect(ctx, GRect(0, y, 3, row_height - 2), 0, GCornerNone);
    }

    // Draw row separator line
    graphics_context_set_stroke_color(ctx, separator_color);
    graphics_draw_line(ctx, GPoint(0, y - 2), GPoint(bounds.size.w - 12, y - 2));

    // Draw Time Column
    graphics_context_set_text_color(ctx, foreground);
    graphics_draw_text(ctx, s_forecast_data[idx][0], fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[0], y + 1, 35, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    
    // Draw Condition Icon
    if(s_icon_bitmaps[idx]) {
      graphics_context_set_compositing_mode(ctx, GCompOpSet);
      graphics_draw_bitmap_in_rect(ctx, s_icon_bitmaps[idx], GRect(x_pos[1], y + 1, 30, 30));
    }

    // Draw Temperature Column (Semantic High/Low Colors)
    int temp_val = atoi(s_forecast_data[idx][1]);
    GColor temp_color = foreground;
    if (!s_is_metric) { // Fahrenheit logic
      if (temp_val >= 90) temp_color = GColorRed;
      else if (temp_val <= 32) temp_color = GColorBlue;
    } else {            // Celsius logic
      if (temp_val >= 32) temp_color = GColorRed;
      else if (temp_val <= 0) temp_color = GColorBlue;
    }
    graphics_context_set_text_color(ctx, temp_color);
    graphics_draw_text(ctx, s_forecast_data[idx][1], fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[2], y + 1, 40, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    // Draw Wind Column (High Wind Warning Color)
    int wind_val = atoi(s_forecast_data[idx][2]);
    graphics_context_set_text_color(ctx, (wind_val >= (s_wind_unit == 1 ? 32 : s_wind_unit == 2 ? 9 : s_wind_unit == 3 ? 17 : 20)) ? GColorOrange : foreground);
    graphics_draw_text(ctx, s_forecast_data[idx][2], fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[3], y + 1, 38, 30), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // Draw Rain Probability Pie Chart
    int rain_val = atoi(s_forecast_data[idx][3]); 
    GRect rain_rect = GRect(x_pos[4], y + 5, 22, 22); // Bounding box for the circle
    
    // Draw the empty background ring
    graphics_context_set_stroke_color(ctx, dark ? GColorLightGray : GColorDarkGray);
    graphics_draw_arc(ctx, rain_rect, GOvalScaleModeFitCircle, 0, TRIG_MAX_ANGLE);
    
    if (rain_val > 0) {
      // Calculate wedge size and draw the filled arc
      graphics_context_set_fill_color(ctx, GColorPictonBlue);
      int32_t rain_angle = (TRIG_MAX_ANGLE * rain_val) / 100;
      graphics_fill_radial(ctx, rain_rect, GOvalScaleModeFitCircle, 11, 0, rain_angle); // 11 is radius inset
    }

    // Draw UV Safety Bar on the far right edge
    GColor uv_color = GColorGreen;
    int uv = s_uv_values[idx];
    if(uv > 2) uv_color = GColorYellow;
    if(uv > 5) uv_color = GColorOrange;
    if(uv > 7) uv_color = GColorRed;
    if(uv > 10) uv_color = GColorPurple;
    graphics_context_set_fill_color(ctx, uv_color);
    graphics_fill_rect(ctx, GRect(bounds.size.w - 10, y + 1, 8, row_height - 2), 2, GCornersAll); // 8px wide, 2px rounded corners
  }

  // 6. Draw Scroll Track Indicator
  graphics_context_set_fill_color(ctx, separator_color); // Track background
  graphics_fill_rect(ctx, GRect(bounds.size.w - 2, 24, 2, bounds.size.h - 24 - bottom_margin), 0, GCornerNone);
  
  // Calculate relative handle position based on offset
  int max_scroll = s_forecast_count > 6 ? s_forecast_count - 6 : 1;
  int scroll_handle_y = 24 + ((s_scroll_offset * (bounds.size.h - 54 - bottom_margin)) / max_scroll);
  graphics_context_set_fill_color(ctx, dark ? GColorWhite : GColorDarkGray);  // Moving handle
  graphics_fill_rect(ctx, GRect(bounds.size.w - 2, scroll_handle_y, 2, 30), 0, GCornerNone);

  // 7. Render Temporary Location Banner (Bottom of screen)
  if (s_show_banner) {
    graphics_context_set_fill_color(ctx, GColorMidnightGreen);
    graphics_fill_rect(ctx, GRect(0, bounds.size.h - 20, bounds.size.w, 20), 0, GCornerNone);
    
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_location_name, fonts_get_system_font(FONT_KEY_GOTHIC_14), 
                       GRect(6, bounds.size.h - 20, bounds.size.w - 12, 16), 
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
  if (s_status[0]) {
    graphics_context_set_fill_color(ctx, GColorMidnightGreen);
    graphics_fill_rect(ctx, GRect(0, bounds.size.h - 20, bounds.size.w, 20), 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_status, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(6, bounds.size.h - 20, bounds.size.w - 12, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

// ==============================================================================
// BLUETOOTH COMMUNICATIONS (APP MESSAGES)
// ==============================================================================

// Callback triggered whenever the phone sends a chunk of data
static int s_last_acked_part = 0;

static void forecast_outbox_sent(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Watch outbox message delivered");
}
static void forecast_outbox_failed(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Watch outbox delivery FAILED: %d", reason);
}
static void forecast_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Watch inbox message DROPPED: %d", reason);
}

static void acknowledge_forecast_part(int number) {
  DictionaryIterator *out = NULL;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK || !out) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "ACK %d outbox unavailable: %d", number, result);
    return;
  }
  dict_write_uint8(out, MESSAGE_KEY_REFRESH_REQUEST, 100 + number);
  result = app_message_outbox_send();
  APP_LOG(result == APP_MSG_OK ? APP_LOG_LEVEL_INFO : APP_LOG_LEVEL_WARNING,
          "ACK part %d: %d", number, result);
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *part = dict_find(iterator, MESSAGE_KEY_TRANSFER_PART);
  Tuple *status = dict_find(iterator, MESSAGE_KEY_STATUS_TEXT);
  if (status) { show_status(status->value->cstring); return; }
  if (!part) {
    Tuple *theme = dict_find(iterator, MESSAGE_KEY_COLOR_MODE);
    if (theme && theme->type == TUPLE_CSTRING) {
      const char *mode = theme->value->cstring;
      s_color_mode = strcmp(mode, "day") == 0 ? 1 : strcmp(mode, "night") == 0 ? 2 : 0;
      s_last_auto_dark = is_dark_mode();
      layer_mark_dirty(s_canvas_layer);
    }
    return;
  }
  int number = (int)part->value->int32;
  if (number < 1 || number > 8) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Invalid forecast part %d", number);
    return;
  }
  // Retry of an already accepted part, including the FINAL part.
  if (number == s_last_acked_part && (number != 1 || s_pending_first)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Duplicate forecast part %d; re-ACK", number);
    acknowledge_forecast_part(number);
    return;
  }
  if (number == 1) {
    s_last_acked_part = 0;
    memset(s_pending_data, 0, sizeof(s_pending_data));
    memset(s_pending_uv, 0, sizeof(s_pending_uv));
    memset(s_pending_night, 0, sizeof(s_pending_night));
    memset(s_pending_conditions, 0, sizeof(s_pending_conditions));
    memset(s_pending_gust, 0, sizeof(s_pending_gust));
    s_pending_first = true;
    s_pending_next_part = 2;
    s_pending_count = 0;
    s_pending_location[0] = '\0';
  } else if (!s_pending_first || number > 8 || (number != s_pending_next_part && number != s_pending_next_part - 1)) return;
  if (number != 1 && number == s_pending_next_part - 1) {
    acknowledge_forecast_part(number);
    return;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "Receiving forecast part %d", number);
  Tuple *t = dict_read_first(iterator);
  while (t) {
    uint32_t key = t->key;
    if (key == MESSAGE_KEY_LOCATION_NAME) snprintf(s_pending_location, sizeof(s_pending_location), "%s", t->value->cstring);
    else if (key == MESSAGE_KEY_COLOR_MODE) {
      const char *mode = t->value->cstring;
      s_pending_color_mode = strcmp(mode, "day") == 0 ? 1 : strcmp(mode, "night") == 0 ? 2 : 0;
    }
    else if (key == MESSAGE_KEY_UNITS) s_pending_metric = strcmp(t->value->cstring, "metric") == 0;
    else if (key == MESSAGE_KEY_WIND_UNIT) {
      const char *unit = t->value->cstring;
      s_pending_wind_unit = strcmp(unit, "kmh") == 0 ? 1 : strcmp(unit, "ms") == 0 ? 2 : strcmp(unit, "knots") == 0 ? 3 : 0;
    }
    else if (key == MESSAGE_KEY_SUNRISE) s_pending_sunrise = (time_t)t->value->int32;
    else if (key == MESSAGE_KEY_SUNSET) s_pending_sunset = (time_t)t->value->int32;
    else if (key == MESSAGE_KEY_FORECAST_TIMESTAMP) s_pending_timestamp = (time_t)t->value->int32;
    else if (key == MESSAGE_KEY_FORECAST_CACHED) s_pending_cached = t->value->int32 != 0;
    else if (key == MESSAGE_KEY_GUSTS_ENABLED) s_pending_gust_enabled = t->value->int32 != 0;
    else if (key == MESSAGE_KEY_TRANSFER_COUNT || key == MESSAGE_KEY_FORECAST_COUNT) s_pending_count = (int)t->value->int32;
    else if (key >= MESSAGE_KEY_IS_NIGHT && key < MESSAGE_KEY_IS_NIGHT + 48) s_pending_night[key-MESSAGE_KEY_IS_NIGHT] = t->value->int32;
    else if (key >= MESSAGE_KEY_COLUMN_COND && key < MESSAGE_KEY_COLUMN_COND + 48) s_pending_conditions[key-MESSAGE_KEY_COLUMN_COND] = t->value->int32;
    else if (key >= MESSAGE_KEY_COLUMN_TIME && key < MESSAGE_KEY_COLUMN_TIME + 48) snprintf(s_pending_data[key-MESSAGE_KEY_COLUMN_TIME][0], 10, "%s", t->value->cstring);
    else if (key >= MESSAGE_KEY_COLUMN_TEMP && key < MESSAGE_KEY_COLUMN_TEMP + 48) snprintf(s_pending_data[key-MESSAGE_KEY_COLUMN_TEMP][1], 10, "%s", t->value->cstring);
    else if (key >= MESSAGE_KEY_COLUMN_WIND && key < MESSAGE_KEY_COLUMN_WIND + 48) snprintf(s_pending_data[key-MESSAGE_KEY_COLUMN_WIND][2], 10, "%s", t->value->cstring);
    else if (key >= MESSAGE_KEY_COLUMN_RAIN && key < MESSAGE_KEY_COLUMN_RAIN + 48) snprintf(s_pending_data[key-MESSAGE_KEY_COLUMN_RAIN][3], 10, "%s", t->value->cstring);
    else if (key >= MESSAGE_KEY_COLUMN_UV && key < MESSAGE_KEY_COLUMN_UV + 48) s_pending_uv[key-MESSAGE_KEY_COLUMN_UV] = t->value->int32;
    else if (key >= MESSAGE_KEY_COLUMN_GUST && key < MESSAGE_KEY_COLUMN_GUST + 48) s_pending_gust[key-MESSAGE_KEY_COLUMN_GUST] = atoi(t->value->cstring);
    t = dict_read_next(iterator);
  }
  int total = s_pending_count;
  if (total < 1 || total > 48) { APP_LOG(APP_LOG_LEVEL_WARNING, "Invalid forecast count %d in part %d", total, number); return; }
  int start = (number - 1) * 6;
  int end = start + 6 < total ? start + 6 : total;
  if (start >= total) { APP_LOG(APP_LOG_LEVEL_WARNING, "Part %d outside count %d", number, total); return; }
  for (int i = start; i < end; i++) {
    if (!s_pending_data[i][0][0] || !s_pending_data[i][1][0] ||
        !s_pending_data[i][2][0] || !s_pending_data[i][3][0]) { APP_LOG(APP_LOG_LEVEL_WARNING, "Missing data in part %d hour %d", number, i); return; }
  }
  for (int i = start; i < end; i++) {
    invalidate_icon(i);
    memcpy(s_forecast_data[i], s_pending_data[i], sizeof(s_forecast_data[i]));
    s_uv_values[i] = s_pending_uv[i];
    s_is_night[i] = s_pending_night[i];
    s_conditions[i] = s_pending_conditions[i];
    s_gust[i] = s_pending_gust[i];
  }
  s_forecast_count = end;
  if (number == 1) {
    s_is_metric = s_pending_metric;
    s_wind_unit = s_pending_wind_unit;
    s_color_mode = s_pending_color_mode;
    s_sunrise = s_pending_sunrise;
    s_sunset = s_pending_sunset;
    s_last_auto_dark = is_dark_mode();
    s_gust_enabled = s_pending_gust_enabled;
    s_forecast_timestamp = s_pending_timestamp;
    s_cached = s_pending_cached;
    if (s_scroll_offset > s_forecast_count - 6) s_scroll_offset = s_forecast_count > 6 ? s_forecast_count - 6 : 0;
    s_has_forecast = true;
    update_icons_for_view();
    if (s_pending_location[0]) {
      snprintf(s_location_name, sizeof(s_location_name), "%s", s_pending_location);
      s_show_banner = true;
      if (s_banner_timer) app_timer_cancel(s_banner_timer);
      s_banner_timer = app_timer_register(3000, banner_timer_callback, NULL);
    }
  }
  s_has_forecast = true;
  s_pending_next_part = number + 1;
  if (end == total) s_pending_first = false;
  update_icons_for_view();
  layer_mark_dirty(s_canvas_layer);
  s_last_acked_part = number;
  acknowledge_forecast_part(number);
  if (end == total) show_status(s_cached ? "Cached forecast" : "Weather updated");
}

// ==============================================================================
// APPLICATION LIFECYCLE
// ==============================================================================

// Fired when the main window pushes to the screen
static void main_window_load(Window *window) {
  // Initialize the canvas layer to fill the screen bounds
  s_canvas_layer = layer_create(layer_get_bounds(window_get_root_layer(window)));
  layer_set_update_proc(s_canvas_layer, canvas_update_proc); // Bind drawing routine
  layer_add_child(window_get_root_layer(window), s_canvas_layer);
}

// App startup
static void init() {
  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) { .load = main_window_load });
  window_stack_push(s_main_window, true);
  
  // Each message carries six hours; acknowledge validated chunks.
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(forecast_inbox_dropped);
  app_message_register_outbox_sent(forecast_outbox_sent);
  app_message_register_outbox_failed(forecast_outbox_failed);
  app_message_open(2048, 256);
  tick_timer_service_subscribe(MINUTE_UNIT, auto_theme_tick); 
}

// App teardown
static void deinit() {
  tick_timer_service_unsubscribe();
  if (s_banner_timer) app_timer_cancel(s_banner_timer); // Clear lingering timers
  if (s_status_timer) app_timer_cancel(s_status_timer);
  for(int i = 0; i < 48; i++) { 
    if(s_icon_bitmaps[i]) gbitmap_destroy(s_icon_bitmaps[i]); // Free graphic RAM
  }
  layer_destroy(s_canvas_layer);
  window_destroy(s_main_window);
}

// Main execution loop
int main(void) { init(); app_event_loop(); deinit(); }