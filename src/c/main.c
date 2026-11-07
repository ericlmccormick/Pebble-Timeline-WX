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
static GBitmap *s_icon_bitmaps[48];     // Shared pointers for icons on visible rows
static uint32_t s_icon_resource_ids[48]; // Resource identity for safe bitmap sharing
// Pending forecasts remain packed until all parts have been verified.
static int s_gust[48], s_forecast_count = 0, s_pending_count = 0;
static int s_pending_wind_unit = 0;
static bool s_pending_metric = false, s_pending_gust_enabled = false, s_gust_enabled = false;
static time_t s_forecast_timestamp = 0, s_pending_timestamp = 0;
static bool s_cached = false, s_pending_cached = false, s_pending_first = false;
static int s_pending_next_part = 0;
static int s_selected_hour = 0;
static char s_pending_location[32] = "";
static char s_status[48] = "";
static bool s_status_error = false;
static bool s_status_critical = false;
static bool s_critical_only = false;
#define WEATHER_CONTROLS_HINT_KEY 7199
#define WEATHER_MESSAGE_POLICY_KEY 7198
#define WEATHER_TIME_FORMAT_KEY 7197
#define WEATHER_GUST_SETTING_KEY 7196
#define WEATHER_APPEARANCE_KEY 7195
static bool s_clock_24 = false;
static Window *s_settings_window = NULL;
static SimpleMenuLayer *s_settings_menu = NULL;
static SimpleMenuItem s_settings_items[5];
static SimpleMenuSection s_settings_sections[1];
#define NOTIFICATION_HEIGHT 20
static AppTimer *s_status_timer = NULL;
static AppTimer *s_slide_timer = NULL;
static int s_notification_offset = NOTIFICATION_HEIGHT; // 0=fully visible, 20=hidden
static bool s_notification_visible = false;
static time_t s_sunrise = 0, s_sunset = 0;
static time_t s_pending_sunrise = 0, s_pending_sunset = 0;
static bool s_last_auto_dark = false;
static int s_color_mode = 0; // 0=auto, 1=day, 2=night
static int s_pending_color_mode = 0;
static int s_wind_unit = 0; // 0=mph, 1=km/h, 2=m/s, 3=knots
static bool s_is_metric = false;
static int s_temp_unit = 0; // 0=F, 1=C, 2=K, 3=R
static int s_pending_temp_unit = 0;        // Tracks user preference for Celsius/kmh vs Fahrenheit/mph

// Navigation and Layout State
static int s_scroll_offset = 0;
static bool s_has_forecast = false; // True after receiving forecast data

// Temporary Notification Banner State
static char s_location_name[32] = "Retrieving weather..."; // Kept for persistence metadata

// ==============================================================================
// MEMORY MANAGEMENT & ASSET LOADING
// ==============================================================================

// Loads only the icons currently visible on the screen to prevent RAM overflow
static void invalidate_icon(int idx) {
  GBitmap *bitmap = s_icon_bitmaps[idx];
  if (!bitmap) return;
  s_icon_bitmaps[idx] = NULL;
  s_icon_resource_ids[idx] = 0;
  for (int i = 0; i < 48; i++) {
    if (s_icon_bitmaps[i] == bitmap) return; // Another row still uses it.
  }
  gbitmap_destroy(bitmap);
}
static void update_icons_for_view() {
  for(int i = 0; i < 48; i++) {
    // Check if the index falls within the currently visible 6-hour window
    if (i >= s_scroll_offset && i < s_scroll_offset + 6) {
      
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
        s_icon_resource_ids[i] = res_id;
        for (int j = 0; j < 48; j++) {
          if (s_icon_bitmaps[j] && s_icon_resource_ids[j] == res_id) {
            s_icon_bitmaps[i] = s_icon_bitmaps[j];
            break;
          }
        }
        if (!s_icon_bitmaps[i]) s_icon_bitmaps[i] = gbitmap_create_with_resource(res_id);
      }
    } else {
      // If the row is off-screen, destroy the bitmap to free up RAM
      invalidate_icon(i);
    }
  }
}

// ==============================================================================
// EVENT HANDLERS & NAVIGATION
// ==============================================================================

// Bottom notification overlay: small, nonblocking slide, never resizes forecast.
static void slide_notification(void *data) {
  s_slide_timer = NULL;
  int target = s_notification_visible ? 0 : NOTIFICATION_HEIGHT;
  if (s_notification_offset < target) {
    s_notification_offset += 5;
    if (s_notification_offset > target) s_notification_offset = target;
  } else if (s_notification_offset > target) {
    s_notification_offset -= 5;
    if (s_notification_offset < target) s_notification_offset = target;
  }
  layer_mark_dirty(s_canvas_layer);
  if (s_notification_offset != target)
    s_slide_timer = app_timer_register(35, slide_notification, NULL);
  else if (!s_notification_visible)
    s_status[0] = '\0';
}
static void animate_notification(bool visible) {
  s_notification_visible = visible;
  if (!s_slide_timer && s_notification_offset != (visible ? 0 : NOTIFICATION_HEIGHT))
    s_slide_timer = app_timer_register(35, slide_notification, NULL);
  layer_mark_dirty(s_canvas_layer);
}
static void hide_notification(void) {
  if (s_status_timer) { app_timer_cancel(s_status_timer); s_status_timer = NULL; }
  s_status_critical = false;
  animate_notification(false);
}
static void status_timer_callback(void *data) {
  s_status_timer = NULL;
  if (!s_status_critical) animate_notification(false);
}
static void show_notification(const char *message, bool critical) {
  if (!message || !message[0]) return;
  if (!critical && (s_critical_only || s_status_critical)) return;
  if (s_status_timer) { app_timer_cancel(s_status_timer); s_status_timer = NULL; }
  snprintf(s_status, sizeof(s_status), "%s", message);
  s_status_error = critical;
  s_status_critical = critical;
  animate_notification(true);
  if (!critical)
    s_status_timer = app_timer_register(4000, status_timer_callback, NULL);
}
static void show_status(const char *message) {
  show_notification(message, false);
}
static void set_notification_policy(bool critical_only) {
  s_critical_only = critical_only;
  persist_write_bool(WEATHER_MESSAGE_POLICY_KEY, critical_only);
  if (critical_only && s_notification_visible && !s_status_critical)
    hide_notification();
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

// Watch-side settings. Long SELECT opens this native Pebble menu;
// refresh remains available as the final menu item.
static void send_watch_preference(uint32_t key, const char *value) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK || !out) return;
  dict_write_cstring(out, key, value);
  dict_write_end(out);
  app_message_outbox_send();
}
static void update_settings_subtitles(void) {
  s_settings_items[0].subtitle = s_clock_24 ? "24-hour" : "12-hour";
  s_settings_items[1].subtitle = s_color_mode == 1 ? "Day" :
      s_color_mode == 2 ? "Night" : "Auto";
  s_settings_items[2].subtitle = s_gust_enabled ? "On" : "Off";
  s_settings_items[3].subtitle = s_critical_only ? "Critical only" : "All";
  if (s_settings_menu)
    layer_mark_dirty(simple_menu_layer_get_layer(s_settings_menu));
}
static void settings_item_selected(int index, void *context) {
  switch (index) {
    case 0:
      s_clock_24 = !s_clock_24;
      persist_write_bool(WEATHER_TIME_FORMAT_KEY, s_clock_24);
      send_watch_preference(MESSAGE_KEY_TIME_FORMAT, s_clock_24 ? "24h" : "12h");
      break;
    case 1:
      s_color_mode = (s_color_mode + 1) % 3;
      persist_write_int(WEATHER_APPEARANCE_KEY, s_color_mode);
      s_last_auto_dark = is_dark_mode();
      layer_mark_dirty(s_canvas_layer);
      send_watch_preference(MESSAGE_KEY_COLOR_MODE,
          s_color_mode == 1 ? "day" : s_color_mode == 2 ? "night" : "auto");
      break;
    case 2:
      s_gust_enabled = !s_gust_enabled;
      persist_write_bool(WEATHER_GUST_SETTING_KEY, s_gust_enabled);
      send_watch_preference(MESSAGE_KEY_SHOW_GUSTS, s_gust_enabled ? "1" : "0");
      break;
    case 3:
      set_notification_policy(!s_critical_only);
      send_watch_preference(MESSAGE_KEY_MESSAGE_POLICY,
          s_critical_only ? "critical" : "all");
      break;
    case 4:
      refresh_click_handler(NULL, NULL);
      if (s_settings_window) window_stack_pop(true);
      return;
  }
  update_settings_subtitles();
}
static void settings_window_unload(Window *window) {
  if (s_settings_menu) {
    simple_menu_layer_destroy(s_settings_menu);
    s_settings_menu = NULL;
  }
  s_settings_window = NULL;
  window_destroy(window);
}
static void settings_window_load(Window *window) {
  s_settings_items[0] = (SimpleMenuItem){.title="Time format",
      .callback=settings_item_selected};
  s_settings_items[1] = (SimpleMenuItem){.title="Appearance",
      .callback=settings_item_selected};
  s_settings_items[2] = (SimpleMenuItem){.title="Show gusts",
      .callback=settings_item_selected};
  s_settings_items[3] = (SimpleMenuItem){.title="Notifications",
      .callback=settings_item_selected};
  s_settings_items[4] = (SimpleMenuItem){.title="Refresh weather",
      .subtitle="Request latest forecast",.callback=settings_item_selected};
  s_settings_sections[0] = (SimpleMenuSection){
      .title="Timeline WX settings",.num_items=5,.items=s_settings_items};
  s_settings_menu = simple_menu_layer_create(
      layer_get_bounds(window_get_root_layer(window)),window,
      s_settings_sections,1,NULL);
  layer_add_child(window_get_root_layer(window),
      simple_menu_layer_get_layer(s_settings_menu));
  update_settings_subtitles();
}
static void settings_open_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_settings_window) return;
  s_settings_window = window_create();
  window_set_window_handlers(s_settings_window,
      (WindowHandlers){.load=settings_window_load,
                       .unload=settings_window_unload});
  window_stack_push(s_settings_window,true);
}

// Binds hardware buttons to click functions
static void refresh_click_handler(ClickRecognizerRef recognizer, void *context) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK && out) {
    dict_write_uint8(out, MESSAGE_KEY_REFRESH_REQUEST, 1);
    dict_write_end(out);
    app_message_outbox_send();
    show_status("Refreshing weather...");
  }
}
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_status_critical) {
    hide_notification();
    return;
  }
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
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, settings_open_handler, NULL);
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
  // A high-contrast Pebble-style header distinguishes controls from data.
  GColor header_color = dark ? GColorDarkGray : GColorMidnightGreen;
  GColor separator_color = dark ? GColorDarkGray : GColorLightGray;
  // 1. Draw appearance-dependent background
  graphics_context_set_fill_color(ctx, background);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // 2. Define headers and column spacing
  char *temp_label = s_temp_unit == 1 ? "°C" : s_temp_unit == 2 ? "K" : s_temp_unit == 3 ? "°R" : "°F";
  char *wind_label = s_wind_unit == 1 ? "km/h" :
                     s_wind_unit == 2 ? "m/s" :
                     s_wind_unit == 3 ? "kt" : "MPH"; 
  char *headers[] = {"Time", "Cond", temp_label, wind_label, "Rain"};
  int x_pos[] = {5, 42, 85, 122, 166}; // X-coordinates for each column

  // 3. Draw Header Background Bar
  graphics_context_set_fill_color(ctx, header_color);
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, 24), 0, GCornerNone);

  // 4. Draw Header Text. Scale the font down when space is constrained;
  // keep the same five columns and six-row forecast layout.
  graphics_context_set_text_color(ctx, GColorWhite);
  GFont header_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#if defined(PBL_SDK_4)
  if (preferred_content_size() >= PreferredContentSizeLarge)
    header_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#endif
  if (bounds.size.w < 200)
    header_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  for(int i = 0; i < 5; i++) {
    graphics_draw_text(ctx, headers[i], header_font, 
                       GRect(x_pos[i], 0, i == 3 ? 38 : 42, 24), GTextOverflowModeTrailingEllipsis, i == 3 ? GTextAlignmentCenter : GTextAlignmentLeft, NULL);
  }

  // Determine how many rows to draw based on banner visibility (to prevent overlapping)
  int visible_rows = 6;
  if (visible_rows > s_forecast_count - s_scroll_offset) visible_rows = s_forecast_count - s_scroll_offset;
  if (visible_rows < 1) visible_rows = 1;
  int bottom_margin = 0; // Notifications slide over the grid without resizing it
  const int grid_height = bounds.size.h - 24 - bottom_margin;
  const int row_height = grid_height / visible_rows;

  // 5. Draw the Data Grid
  for(int i = 0; i < (s_has_forecast ? visible_rows : 0); i++) {
    int idx = i + s_scroll_offset;    // The actual data array index based on scroll position
    int y = 24 + (i * grid_height) / visible_rows; // Distribute rows evenly to the bottom
    
    // A quiet fill and narrow blue rule mark the current forecast hour.
    if (idx == 0) {
      graphics_context_set_fill_color(ctx, dark ? GColorDarkGray : GColorLightGray);
      graphics_fill_rect(ctx, GRect(0, y, bounds.size.w - 3, row_height), 0, GCornerNone);
      graphics_context_set_fill_color(ctx, GColorPictonBlue);
      graphics_fill_rect(ctx, GRect(0, y, 3, row_height - 2), 0, GCornerNone);
    }

    // Draw row separator line
    graphics_context_set_stroke_color(ctx, separator_color);
    graphics_draw_line(ctx, GPoint(0, y - 2), GPoint(bounds.size.w - 12, y - 2));

    // Draw Time Column
    graphics_context_set_text_color(ctx, foreground);
    GFont row_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    GFont small_row_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    GFont accessible_number_font = row_font;
#if defined(PBL_SDK_4)
    // Use the larger system font for short numeric values when the screen
    // has enough room; retain normal font for long values to prevent clipping.
    if (preferred_content_size() >= PreferredContentSizeLarge && row_height >= 34)
      accessible_number_font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
#endif
    graphics_draw_text(ctx, s_forecast_data[idx][0], row_font, 
                       GRect(x_pos[0], y + 1, 35, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    
    // Draw Condition Icon
    if(s_icon_bitmaps[idx]) {
      graphics_context_set_compositing_mode(ctx, GCompOpSet);
      graphics_draw_bitmap_in_rect(ctx, s_icon_bitmaps[idx], GRect(x_pos[1], y + 1, 30, 30));
    }

    // Draw Temperature Column (Semantic High/Low Colors)
    int temp_val = atoi(s_forecast_data[idx][1]);
    GColor temp_color = foreground;
    // Color thresholds represent 90°F hot / 32°F freezing in all units.
    int hot_threshold = s_temp_unit == 1 ? 32 : s_temp_unit == 2 ? 305 : s_temp_unit == 3 ? 550 : 90;
    int cold_threshold = s_temp_unit == 1 ? 0 : s_temp_unit == 2 ? 273 : s_temp_unit == 3 ? 492 : 32;
    if (temp_val >= hot_threshold) temp_color = GColorRed;
    else if (temp_val <= cold_threshold) temp_color = GColorBlue;
    graphics_context_set_text_color(ctx, temp_color);
    graphics_draw_text(ctx, s_forecast_data[idx][1],
                       strlen(s_forecast_data[idx][1]) >= 4 ? small_row_font :
                       strlen(s_forecast_data[idx][1]) <= 2 ? accessible_number_font : row_font, 
                       GRect(x_pos[2], y + 1, 40, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    // Draw Wind Column (High Wind Warning Color)
    int wind_val = atoi(s_forecast_data[idx][2]);
    graphics_context_set_text_color(ctx, (wind_val >= (s_wind_unit == 1 ? 32 : s_wind_unit == 2 ? 9 : s_wind_unit == 3 ? 17 : 20)) ? GColorOrange : foreground);
    graphics_draw_text(ctx, s_forecast_data[idx][2],
                       strlen(s_forecast_data[idx][2]) >= 4 ? small_row_font :
                       strlen(s_forecast_data[idx][2]) <= 2 ? accessible_number_font : row_font, 
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
    int uv = s_uv_values[idx];
    if (uv >= 0) { // -1 means this provider did not supply UV data.
      GColor uv_color = GColorGreen;
      if(uv > 2) uv_color = GColorYellow;
      if(uv > 5) uv_color = GColorOrange;
      if(uv > 7) uv_color = GColorRed;
      if(uv > 10) uv_color = GColorPurple;
      graphics_context_set_fill_color(ctx, uv_color);
      graphics_fill_rect(ctx, GRect(bounds.size.w - 10, y + 1, 8, row_height - 2), 2, GCornersAll);
    } // 8px wide, 2px rounded corners
  }

  // 6. Draw Scroll Track Indicator
  graphics_context_set_fill_color(ctx, separator_color); // Track background
  graphics_fill_rect(ctx, GRect(bounds.size.w - 2, 24, 2, bounds.size.h - 24 - bottom_margin), 0, GCornerNone);
  
  // Calculate relative handle position based on offset
  int max_scroll = s_forecast_count > 6 ? s_forecast_count - 6 : 1;
  int scroll_handle_y = 24 + ((s_scroll_offset * (bounds.size.h - 54 - bottom_margin)) / max_scroll);
  graphics_context_set_fill_color(ctx, dark ? GColorWhite : GColorDarkGray);  // Moving handle
  graphics_fill_rect(ctx, GRect(bounds.size.w - 2, scroll_handle_y, 2, 30), 0, GCornerNone);

  // 7. Render the nonmodal notification LAST, on top of the six-row grid.
  if (s_status[0] && s_notification_offset < NOTIFICATION_HEIGHT) {
    const int notification_y = bounds.size.h - NOTIFICATION_HEIGHT + s_notification_offset;
    graphics_context_set_fill_color(ctx, s_status_error ? GColorRed :
                                    (dark ? GColorDarkGray : GColorMidnightGreen));
    graphics_fill_rect(ctx, GRect(0, notification_y, bounds.size.w, NOTIFICATION_HEIGHT), 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_status, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(6, notification_y + 1, bounds.size.w - 12, NOTIFICATION_HEIGHT - 2),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

// ==============================================================================
// BLUETOOTH COMMUNICATIONS (APP MESSAGES)
// ==============================================================================

// Callback triggered whenever the phone sends a chunk of data

// A compact copy of each accepted part is kept for persistence.
#define WEATHER_CACHE_MAGIC 0x57584333 // v3: numeric-only temperatures and unit metadata
#define WEATHER_CACHE_META_KEY 7100
#define WEATHER_CACHE_PART_KEY 7110
static uint8_t s_pending_packed[8][120];
static uint8_t s_pending_packed_len[8];

typedef struct {
  uint32_t magic;
  uint32_t fingerprint;
  int32_t count;
  int32_t timestamp;
  int32_t sunrise;
  int32_t sunset;
  int32_t wind_unit;
  int32_t color_mode;
  uint8_t metric;
  uint8_t gusts;
  uint8_t cached;
  uint8_t temp_unit;
  char location[32];
} WeatherCacheMeta;

static uint32_t forecast_checksum(int count) {
  uint32_t h = 2166136261u;
  for (int part = 0; part < (count + 5) / 6; part++) {
    for (int i = 0; i < s_pending_packed_len[part]; i++) {
      h ^= s_pending_packed[part][i];
      h *= 16777619u;
    }
  }
  return h;
}

static void decode_forecast_bytes(const uint8_t *data, int count, int offset) {
  for (int r = 0; r < count && offset+r < 48; r++) {
    int i = offset + r;
    const uint8_t *b = data + r * 20;
    memcpy(s_forecast_data[i][0], b, 4);
    s_forecast_data[i][0][4] = '\0';
    memcpy(s_forecast_data[i][1], b + 4, 7);
    s_forecast_data[i][1][7] = '\0';
    unsigned int wind = b[11] | ((unsigned int)b[12] << 8);
    unsigned int gust = b[13] | ((unsigned int)b[14] << 8);
    snprintf(s_forecast_data[i][2], 10, "%u", wind);
    if (b[15] == 255) snprintf(s_forecast_data[i][3], 10, "--");
    else snprintf(s_forecast_data[i][3], 10, "%u%%", b[15]);
    s_uv_values[i] = b[16] == 255 ? -1 : (int)b[16];
    s_conditions[i] = b[17] | ((int)b[18] << 8);
    s_is_night[i] = b[19] ? 1 : 0;
    s_gust[i] = (int)gust;
  }
}

static void store_complete_forecast(void) {
  if (s_forecast_count < 1 || s_forecast_count > 48) return;
  WeatherCacheMeta meta = {
    .magic = WEATHER_CACHE_MAGIC, .fingerprint = forecast_checksum(s_forecast_count),
    .count = s_forecast_count, .timestamp = (int32_t)s_forecast_timestamp,
    .sunrise = (int32_t)s_sunrise, .sunset = (int32_t)s_sunset,
    .wind_unit = s_wind_unit, .color_mode = s_color_mode,
    .metric = s_is_metric, .gusts = s_gust_enabled, .cached = s_cached,
    .temp_unit = (uint8_t)s_temp_unit
  };
  snprintf(meta.location, sizeof(meta.location), "%s", s_location_name);
  // Compare against the stored blocks first: avoid unnecessary flash writes.
  // Write the checksum-bearing manifest last so a partly updated cache is rejected.
  uint8_t existing[120];
  int changed_parts = 0;
  for (int i = 0; i < (s_forecast_count + 5) / 6; i++) {
    int size = s_pending_packed_len[i];
    int prior_size = persist_read_data(WEATHER_CACHE_PART_KEY + i, existing, sizeof(existing));
    if (prior_size == size && memcmp(existing, s_pending_packed[i], size) == 0) continue;
    if (persist_write_data(WEATHER_CACHE_PART_KEY + i,
                           s_pending_packed[i], size) != size) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Unable to persist forecast part %d", i);
      return;
    }
    changed_parts++;
  }
  WeatherCacheMeta old_meta;
  int old_size = persist_read_data(WEATHER_CACHE_META_KEY, &old_meta, sizeof(old_meta));
  if (old_size != sizeof(old_meta) || memcmp(&old_meta, &meta, sizeof(meta)) != 0) {
    if (persist_write_data(WEATHER_CACHE_META_KEY, &meta, sizeof(meta)) != sizeof(meta))
      APP_LOG(APP_LOG_LEVEL_WARNING, "Unable to persist forecast metadata");
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Persisted %d changed weather parts", changed_parts);
}

static void restore_persisted_forecast(void) {
  WeatherCacheMeta meta;
  if (persist_read_data(WEATHER_CACHE_META_KEY, &meta, sizeof(meta)) != sizeof(meta) ||
      meta.magic != WEATHER_CACHE_MAGIC || meta.count < 1 || meta.count > 48) return;
  int parts = (meta.count + 5) / 6;
  for (int i = 0; i < parts; i++) {
    int expected = (meta.count - i * 6) * 20;
    if (expected > 120) expected = 120;
    int received = persist_read_data(WEATHER_CACHE_PART_KEY+i, s_pending_packed[i], expected);
    if (received != expected) return;
    s_pending_packed_len[i] = expected;
  }
  if (forecast_checksum(meta.count) != meta.fingerprint) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Persisted forecast checksum mismatch");
    return;
  }
  for (int p = 0; p < parts; p++)
    decode_forecast_bytes(s_pending_packed[p], s_pending_packed_len[p]/20, p*6);
  s_forecast_count = meta.count;
  s_forecast_timestamp = (time_t)meta.timestamp;
  s_sunrise = (time_t)meta.sunrise;
  s_sunset = (time_t)meta.sunset;
  s_wind_unit = meta.wind_unit;
  s_color_mode = meta.color_mode;
  s_is_metric = meta.metric != 0;
  s_temp_unit = meta.temp_unit <= 3 ? meta.temp_unit : 0;
  s_gust_enabled = meta.gusts != 0;
  s_cached = true;
  s_has_forecast = true;
  snprintf(s_location_name, sizeof(s_location_name), "%s", meta.location);
  s_last_auto_dark = is_dark_mode();
  update_icons_for_view();
  layer_mark_dirty(s_canvas_layer);
  APP_LOG(APP_LOG_LEVEL_INFO, "Restored %d forecast hours from watch storage", meta.count);
}

static int s_last_acked_part = 0;
static int32_t s_pending_transfer_id = 0;
static int32_t s_last_acked_transfer_id = 0;

static void forecast_outbox_sent(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Watch outbox message delivered");
}
static void forecast_outbox_failed(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Watch outbox delivery FAILED: %d", reason);
}
static void forecast_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Watch inbox message DROPPED: %d", reason);
}

static void acknowledge_forecast_part(int number, int32_t transfer_id) {
  DictionaryIterator *out = NULL;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK || !out) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "ACK %d outbox unavailable: %d", number, result);
    return;
  }
  dict_write_uint8(out, MESSAGE_KEY_REFRESH_REQUEST, 100 + number);
  dict_write_int32(out, MESSAGE_KEY_TRANSFER_ID, transfer_id);
  result = app_message_outbox_send();
  APP_LOG(result == APP_MSG_OK ? APP_LOG_LEVEL_INFO : APP_LOG_LEVEL_WARNING,
          "ACK part %d: %d", number, result);
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *part = dict_find(iterator, MESSAGE_KEY_TRANSFER_PART);
  Tuple *status = dict_find(iterator, MESSAGE_KEY_STATUS_TEXT);
  if (status && status->type == TUPLE_CSTRING) {
    Tuple *level = dict_find(iterator, MESSAGE_KEY_STATUS_LEVEL);
    show_notification(status->value->cstring, level && level->value->int32 == 1);
    return;
  }
  if (!part) {
    Tuple *policy = dict_find(iterator, MESSAGE_KEY_MESSAGE_POLICY);
    if (policy && policy->type == TUPLE_CSTRING)
      set_notification_policy(strcmp(policy->value->cstring, "critical") == 0);
    Tuple *clock = dict_find(iterator, MESSAGE_KEY_TIME_FORMAT);
    if (clock && clock->type == TUPLE_CSTRING) {
      s_clock_24 = strcmp(clock->value->cstring, "24h") == 0;
      persist_write_bool(WEATHER_TIME_FORMAT_KEY, s_clock_24);
    }
    Tuple *gust = dict_find(iterator, MESSAGE_KEY_SHOW_GUSTS);
    if (gust) {
      s_gust_enabled = gust->type == TUPLE_CSTRING ?
          (strcmp(gust->value->cstring,"1")==0 ||
           strcmp(gust->value->cstring,"true")==0) :
          gust->value->int32 != 0;
      persist_write_bool(WEATHER_GUST_SETTING_KEY,s_gust_enabled);
    }
    Tuple *theme = dict_find(iterator, MESSAGE_KEY_COLOR_MODE);
    if (theme && theme->type == TUPLE_CSTRING) {
      const char *mode = theme->value->cstring;
      s_color_mode = strcmp(mode, "day") == 0 ? 1 : strcmp(mode, "night") == 0 ? 2 : 0;
      s_last_auto_dark = is_dark_mode();
      persist_write_int(WEATHER_APPEARANCE_KEY,s_color_mode);
      layer_mark_dirty(s_canvas_layer);
    }
    if(s_settings_menu)update_settings_subtitles();
    return;
  }
  Tuple *id_tuple = dict_find(iterator, MESSAGE_KEY_TRANSFER_ID);
  if (!id_tuple) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Forecast chunk without transaction ID");
    return;
  }
  int32_t transfer_id = id_tuple->value->int32;
  int number = (int)part->value->int32;
  if (number < 1 || number > 8) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Invalid forecast part %d", number);
    return;
  }
  // Retry of an already accepted part, including the FINAL part.
  if (transfer_id == s_last_acked_transfer_id && number == s_last_acked_part) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Duplicate forecast part %d; re-ACK", number);
    acknowledge_forecast_part(number, transfer_id);
    return;
  }
  if (number == 1) {
    s_pending_transfer_id = transfer_id;
    s_last_acked_part = 0;
    s_pending_first = true;
    s_pending_next_part = 2;
    s_pending_count = 0;
    s_pending_temp_unit = 0;
    s_pending_location[0] = '\0';
    memset(s_pending_packed_len, 0, sizeof(s_pending_packed_len));
  } else if (!s_pending_first || transfer_id != s_pending_transfer_id || (number != s_pending_next_part && number != s_pending_next_part - 1)) return;
  if (number != 1 && number == s_pending_next_part - 1) {
    acknowledge_forecast_part(number, transfer_id);
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
    else if (key == MESSAGE_KEY_TEMP_UNIT && t->type == TUPLE_CSTRING) {
      const char *unit = t->value->cstring;
      s_pending_temp_unit = strcmp(unit, "C") == 0 ? 1 : strcmp(unit, "K") == 0 ? 2 : strcmp(unit, "R") == 0 ? 3 : 0;
    }
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
    t = dict_read_next(iterator);
  }
  Tuple *blob = dict_find(iterator, MESSAGE_KEY_FORECAST_BLOB);
  int total = s_pending_count;
  if (total < 1 || total > 48) { APP_LOG(APP_LOG_LEVEL_WARNING, "Invalid count %d", total); return; }
  int start = (number - 1) * 6;
  int end = start + 6 < total ? start + 6 : total;
  if (start >= total || !blob || blob->type != TUPLE_BYTE_ARRAY ||
      blob->length != (end-start)*20) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Invalid packed forecast part %d", number);
    return;
  }
  const uint8_t *bytes = blob->value->data;
  for (int i = 0; i < end-start; i++) {
    const uint8_t *b = bytes + 20*i;
    if (!b[0] || !b[4] || (b[15] > 100 && b[15] != 255) || b[19] > 1) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Invalid forecast row %d", start+i);
      return;
    }
  }
  memcpy(s_pending_packed[number-1], bytes, blob->length);
  s_pending_packed_len[number-1] = blob->length;
  // Keep the last complete forecast visible until ALL chunks arrive.
  // Never expose a partial refresh to the scrolling UI.
  s_pending_next_part = number + 1;
  if (end == total) {
    // Every chunk has already been checked: decode the complete snapshot at once.
    // The previous displayed forecast stays untouched until this point.
    for (int i = 0; i < total; i++) {
      const uint8_t *b = s_pending_packed[i/6] + (i%6)*20;
      int condition = b[17] | ((int)b[18]<<8);
      if (s_conditions[i] != condition || s_is_night[i] != (b[19] ? 1 : 0))
        invalidate_icon(i);
    }
    for (int p = 0; p < (total+5)/6; p++)
      decode_forecast_bytes(s_pending_packed[p], s_pending_packed_len[p]/20, p*6);
    s_forecast_count = total;
    s_is_metric = s_pending_metric;
    s_temp_unit = s_pending_temp_unit;
    s_wind_unit = s_pending_wind_unit;
    s_color_mode = s_pending_color_mode;
    s_sunrise = s_pending_sunrise;
    s_sunset = s_pending_sunset;
    s_last_auto_dark = is_dark_mode();
    s_gust_enabled = s_pending_gust_enabled;
    s_forecast_timestamp = s_pending_timestamp;
    s_cached = s_pending_cached;
    if (s_scroll_offset > total - 6) s_scroll_offset = total > 6 ? total - 6 : 0;
    s_has_forecast = true;
    s_pending_first = false;
    update_icons_for_view();
    layer_mark_dirty(s_canvas_layer);
    if (s_pending_location[0]) {
      snprintf(s_location_name, sizeof(s_location_name), "%s", s_pending_location);
      show_status(s_pending_location);
    }
    store_complete_forecast();
  }
  s_last_acked_part = number;
  s_last_acked_transfer_id = transfer_id;
  acknowledge_forecast_part(number, transfer_id);
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
  if (persist_exists(WEATHER_TIME_FORMAT_KEY))
    s_clock_24 = persist_read_bool(WEATHER_TIME_FORMAT_KEY);
  if (persist_exists(WEATHER_GUST_SETTING_KEY))
    s_gust_enabled = persist_read_bool(WEATHER_GUST_SETTING_KEY);
  if (persist_exists(WEATHER_APPEARANCE_KEY))
    s_color_mode = persist_read_int(WEATHER_APPEARANCE_KEY);
  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) { .load = main_window_load });
  window_stack_push(s_main_window, true);
  restore_persisted_forecast();
  if (persist_exists(WEATHER_MESSAGE_POLICY_KEY))
    s_critical_only = persist_read_bool(WEATHER_MESSAGE_POLICY_KEY);
  if (!persist_exists(WEATHER_CONTROLS_HINT_KEY)) {
    show_status("Hold SELECT for settings");
    persist_write_bool(WEATHER_CONTROLS_HINT_KEY, true);
  }

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
  if (s_status_timer) app_timer_cancel(s_status_timer);
  if (s_slide_timer) app_timer_cancel(s_slide_timer);
  for(int i = 0; i < 48; i++) { 
    invalidate_icon(i); // Free shared bitmaps only after their last reference
  }
  layer_destroy(s_canvas_layer);
  window_destroy(s_main_window);
}

// Main execution loop
int main(void) { init(); app_event_loop(); deinit(); }