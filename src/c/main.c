#include <pebble.h>

#define PERSIST_KEY_CONFIG_V2 150
#define PERSIST_KEY_DATA_START 200 

// ==============================================================================
// GLOBALS & STRUCTURES
// ==============================================================================
static Window *s_main_window;
static Window *s_settings_window;
static SimpleMenuLayer *s_settings_menu_layer;
static SimpleMenuSection s_menu_sections[1];
static SimpleMenuItem s_menu_items[5];

static Layer *s_bg_layer;       
static Layer *s_data_layer;     
static Layer *s_banner_layer;   

typedef struct {
  time_t timestamp;
  int16_t temp;
  int16_t wind;
  uint8_t rain;
  uint8_t uv;
  uint16_t condition;
  uint8_t is_night;
  uint8_t hazard;
  uint8_t day_break;
} WeatherHour;

static WeatherHour s_forecast[48];     
static GBitmap *s_icon_bitmaps[48];     
static bool s_chunk_loaded[8];       

static uint8_t s_temp_unit = 0; 
static uint8_t s_wind_unit = 0; 
static uint8_t s_time_format = 0; // 0: 12h, 1: 24h
static uint8_t s_dark_mode_setting = 2; // 0: Off, 1: On, 2: Auto
static uint8_t s_provider = 0; // 0: Open-Meteo, 1: NWS, 2: Met.no, 3: OpenWeather

static int s_scroll_offset = 0;         
static int s_valid_start_idx = 0;       

static char *temp_labels[] = {"Fahrenheit (°F)", "Celsius (°C)", "Kelvin (K)", "Rankine (°R)"};
static char *temp_values[] = {"F", "C", "K", "R"};
static char *wind_labels[] = {"mph", "kph", "m/s", "knots"};
static char *wind_values[] = {"mph", "kph", "mps", "knots"};
static char *dark_labels[] = {"Off", "On", "Auto"};
static char *dark_values[] = {"off", "on", "auto"};
static char *time_labels[] = {"12 Hour", "24 Hour"};
static char *time_values[] = {"12h", "24h"};
static char *provider_labels[] = {"Open-Meteo", "NWS (US)", "Met.no", "OpenWeather"};
static char *provider_values[] = {"openmeteo", "nws", "metno", "openweather"};

static char s_location_name[32] = "Locating..."; 
static char s_status_message[32] = "Initializing..."; 
static int s_refresh_interval = 30;
static int s_minutes_to_refresh = 0;
static bool s_is_error_state = false;

// Auto Dark Mode engine relies purely on physical location and forecast time
static bool is_dark() {
  if (s_dark_mode_setting == 1) return true;
  if (s_dark_mode_setting == 0) return false;
  if (s_valid_start_idx < 48 && s_chunk_loaded[s_valid_start_idx / 6]) {
      return s_forecast[s_valid_start_idx].is_night == 1;
  }
  return false;
}

// ==============================================================================
// STORAGE & TIMELINE
// ==============================================================================
static void save_persistent_data() {
  persist_write_data(PERSIST_KEY_CONFIG_V2, &s_dark_mode_setting, sizeof(uint8_t));
  persist_write_data(PERSIST_KEY_CONFIG_V2 + 1, &s_time_format, sizeof(uint8_t));
  persist_write_data(PERSIST_KEY_CONFIG_V2 + 2, &s_temp_unit, sizeof(uint8_t));
  persist_write_data(PERSIST_KEY_CONFIG_V2 + 3, &s_wind_unit, sizeof(uint8_t));
  persist_write_data(PERSIST_KEY_CONFIG_V2 + 4, &s_provider, sizeof(uint8_t));
  for (int i = 0; i < 8; i++) persist_write_bool(PERSIST_KEY_CONFIG_V2 + 10 + i, s_chunk_loaded[i]);
  for (int i = 0; i < 48; i++) persist_write_data(PERSIST_KEY_DATA_START + i, &s_forecast[i], sizeof(WeatherHour));
}

static void load_persistent_data() {
  if (persist_exists(PERSIST_KEY_CONFIG_V2)) {
    persist_read_data(PERSIST_KEY_CONFIG_V2, &s_dark_mode_setting, sizeof(uint8_t));
    persist_read_data(PERSIST_KEY_CONFIG_V2 + 1, &s_time_format, sizeof(uint8_t));
    persist_read_data(PERSIST_KEY_CONFIG_V2 + 2, &s_temp_unit, sizeof(uint8_t));
    persist_read_data(PERSIST_KEY_CONFIG_V2 + 3, &s_wind_unit, sizeof(uint8_t));
    persist_read_data(PERSIST_KEY_CONFIG_V2 + 4, &s_provider, sizeof(uint8_t));
    for (int i = 0; i < 8; i++) s_chunk_loaded[i] = persist_read_bool(PERSIST_KEY_CONFIG_V2 + 10 + i);
  }
  for (int i = 0; i < 48; i++) {
    if (persist_exists(PERSIST_KEY_DATA_START + i)) persist_read_data(PERSIST_KEY_DATA_START + i, &s_forecast[i], sizeof(WeatherHour));
  }
}

static void calculate_valid_timeline() {
  time_t now = time(NULL);
  s_valid_start_idx = 0;
  for (int i = 0; i < 48; i++) {
    if (s_chunk_loaded[i / 6] && s_forecast[i].timestamp != 0 && s_forecast[i].timestamp > now - 3600) {
      s_valid_start_idx = i;
      break;
    }
  }
}

static void request_chunk(int chunk_index) {
  if (s_chunk_loaded[chunk_index]) return;
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_int32(iter, MESSAGE_KEY_REQUEST_CHUNK, chunk_index);
    app_message_outbox_send();
  }
}

static void update_icons_for_view() {
  for(int i = 0; i < 48; i++) {
    int display_idx = s_valid_start_idx + s_scroll_offset;
    if (i >= display_idx && i < display_idx + 6) {
      if (!s_icon_bitmaps[i] && s_forecast[i].condition != 0 && s_chunk_loaded[i / 6]) {
        bool night = (s_forecast[i].is_night == 1);
        uint32_t res_id;
        int cid = s_forecast[i].condition;

        if (cid == 800) res_id = night ? RESOURCE_ID_ICON_CLEAR_NIGHT : RESOURCE_ID_ICON_CLEAR_DAY;
        else if (cid == 801) res_id = night ? RESOURCE_ID_ICON_CLOUDY_NIGHT : RESOURCE_ID_ICON_CLOUDY_SUN;
        else if (cid >= 200 && cid <= 232) res_id = RESOURCE_ID_ICON_THUNDER_SHOWER;
        else if (cid >= 300 && cid <= 321) res_id = night ? RESOURCE_ID_ICON_SHOWER_NIGHT : RESOURCE_ID_ICON_SHOWER_DAY;
        else if (cid >= 500 && cid <= 501) res_id = RESOURCE_ID_ICON_LIGHT_RAIN;
        else if (cid >= 502 && cid <= 504) res_id = RESOURCE_ID_ICON_HEAVY_RAIN;
        else if (cid == 511) res_id = RESOURCE_ID_ICON_SLEET;
        else if (cid >= 520 && cid <= 531) res_id = RESOURCE_ID_ICON_RAIN_STORM;
        else if (cid == 600) res_id = RESOURCE_ID_ICON_LIGHT_SNOW;
        else if (cid == 601) res_id = RESOURCE_ID_ICON_SNOW;
        else if (cid == 602) res_id = RESOURCE_ID_ICON_HEAVY_SNOW;
        else if (cid >= 620 && cid <= 622) res_id = night ? RESOURCE_ID_ICON_SNOW_SHOWER_NIGHT : RESOURCE_ID_ICON_SNOW_SHOWER_DAY;
        else if (cid >= 701 && cid <= 761) res_id = RESOURCE_ID_ICON_FOG;
        else res_id = RESOURCE_ID_ICON_CLOUDY; 

        s_icon_bitmaps[i] = gbitmap_create_with_resource(res_id);
      }
    } else {
      if (s_icon_bitmaps[i]) {
        gbitmap_destroy(s_icon_bitmaps[i]);
        s_icon_bitmaps[i] = NULL; 
      }
    }
  }
}

// ==============================================================================
// TIMELINE GAP VERIFICATION & RELOAD
// ==============================================================================
static void verify_and_repair_timeline_gaps() {
  for (int chunk_idx = 0; chunk_idx < 8; chunk_idx++) {
    if (!s_chunk_loaded[chunk_idx]) {
      request_chunk(chunk_idx);
    }
  }
}

// ==============================================================================
// WATCH MENU ENGINE
// ==============================================================================
static void temp_select_callback(int index, void *ctx) {
  s_temp_unit = (s_temp_unit + 1) % 4;
  s_menu_items[0].subtitle = temp_labels[s_temp_unit];
  layer_mark_dirty(simple_menu_layer_get_layer(s_settings_menu_layer));
}

static void wind_select_callback(int index, void *ctx) {
  s_wind_unit = (s_wind_unit + 1) % 4;
  s_menu_items[1].subtitle = wind_labels[s_wind_unit];
  layer_mark_dirty(simple_menu_layer_get_layer(s_settings_menu_layer));
}

static void time_select_callback(int index, void *ctx) {
  s_time_format = (s_time_format + 1) % 2;
  s_menu_items[2].subtitle = time_labels[s_time_format];
  layer_mark_dirty(simple_menu_layer_get_layer(s_settings_menu_layer));
}

static void dark_select_callback(int index, void *ctx) {
  s_dark_mode_setting = (s_dark_mode_setting + 1) % 3;
  s_menu_items[3].subtitle = dark_labels[s_dark_mode_setting];
  layer_mark_dirty(simple_menu_layer_get_layer(s_settings_menu_layer));
}

static void provider_select_callback(int index, void *ctx) {
  s_provider = (s_provider + 1) % 4;
  s_menu_items[4].subtitle = provider_labels[s_provider];
  layer_mark_dirty(simple_menu_layer_get_layer(s_settings_menu_layer));
}

static void settings_menu_load(Window *window) {
  s_menu_items[0] = (SimpleMenuItem) { .title = "Temperature", .subtitle = temp_labels[s_temp_unit], .callback = temp_select_callback };
  s_menu_items[1] = (SimpleMenuItem) { .title = "Wind Speed", .subtitle = wind_labels[s_wind_unit], .callback = wind_select_callback };
  s_menu_items[2] = (SimpleMenuItem) { .title = "Time Format", .subtitle = time_labels[s_time_format], .callback = time_select_callback };
  s_menu_items[3] = (SimpleMenuItem) { .title = "Dark Mode", .subtitle = dark_labels[s_dark_mode_setting], .callback = dark_select_callback };
  s_menu_items[4] = (SimpleMenuItem) { .title = "Provider", .subtitle = provider_labels[s_provider], .callback = provider_select_callback };
  
  s_menu_sections[0] = (SimpleMenuSection) { .num_items = 5, .items = s_menu_items, .title = "Local Settings" };
  
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_frame(window_layer);
  s_settings_menu_layer = simple_menu_layer_create(bounds, window, s_menu_sections, 1, NULL);
  layer_add_child(window_layer, simple_menu_layer_get_layer(s_settings_menu_layer));
}

static void settings_menu_unload(Window *window) {
  snprintf(s_status_message, sizeof(s_status_message), "Syncing Settings...");
  layer_mark_dirty(s_banner_layer);

  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_cstring(iter, MESSAGE_KEY_TEMP_UNIT, temp_values[s_temp_unit]);
    dict_write_cstring(iter, MESSAGE_KEY_WIND_UNIT, wind_values[s_wind_unit]);
    dict_write_cstring(iter, MESSAGE_KEY_TIME_FORMAT, time_values[s_time_format]);
    dict_write_cstring(iter, MESSAGE_KEY_DARK_MODE, dark_values[s_dark_mode_setting]);
    dict_write_cstring(iter, MESSAGE_KEY_WEATHER_PROVIDER, provider_values[s_provider]);
    dict_write_uint8(iter, MESSAGE_KEY_SETTINGS_SYNC, 1);
    app_message_outbox_send();
  }

  save_persistent_data();
  simple_menu_layer_destroy(s_settings_menu_layer);
  window_destroy(s_settings_window);
  s_settings_window = NULL;
  
  layer_mark_dirty(s_bg_layer);
  layer_mark_dirty(s_data_layer);
  layer_mark_dirty(s_banner_layer);
}

// ==============================================================================
// MENUS & CLICK HANDLERS
// ==============================================================================
static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_settings_window = window_create();
  window_set_window_handlers(s_settings_window, (WindowHandlers) {
    .load = settings_menu_load,
    .unload = settings_menu_unload,
  });
  window_stack_push(s_settings_window, true);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scroll_offset > 0) {
    s_scroll_offset -= 1;             
    update_icons_for_view();          
    layer_mark_dirty(s_data_layer); 
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_valid_start_idx + s_scroll_offset < 42) {         
    s_scroll_offset += 1;             
    update_icons_for_view();          
    layer_mark_dirty(s_data_layer); 
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (s_minutes_to_refresh > 0) {
    s_minutes_to_refresh--;
    snprintf(s_status_message, sizeof(s_status_message), "Next refresh: %dm", s_minutes_to_refresh);
  } else {
    snprintf(s_status_message, sizeof(s_status_message), "Waiting on server...");
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    dict_write_uint8(iter, 1000, 1); 
    app_message_outbox_send();
    s_minutes_to_refresh = s_refresh_interval;
  }
  calculate_valid_timeline();
  verify_and_repair_timeline_gaps(); 
  layer_mark_dirty(s_banner_layer);
  layer_mark_dirty(s_data_layer);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click_handler, NULL);
}

// ==============================================================================
// DRAWING LAYERS
// ==============================================================================
static void bg_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer); 
  graphics_context_set_fill_color(ctx, is_dark() ? GColorBlack : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  char *headers[] = {"Time", "Cond", "Temp", "Wind", "Rain"};
  int x_pos[] = {5, 42, 85, 122, 160}; 

  graphics_context_set_fill_color(ctx, is_dark() ? GColorDarkGray : GColorLightGray);
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, 24), 0, GCornerNone);

  graphics_context_set_text_color(ctx, is_dark() ? GColorWhite : GColorBlack);
  for(int i = 0; i < 5; i++) {
    graphics_draw_text(ctx, headers[i], fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), 
                       GRect(x_pos[i], 0, 42, 24), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  }
}

static void data_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer); 
  int x_pos[] = {5, 42, 85, 122, 160}; 
  
  graphics_context_set_fill_color(ctx, is_dark() ? GColorBlack : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int current_visible_start_chunk = (s_valid_start_idx + s_scroll_offset) / 6;
  
  for (int c = current_visible_start_chunk; c <= current_visible_start_chunk + 1; c++) {
    if (c < 8 && !s_chunk_loaded[c]) {
      request_chunk(c);
    }
  }

  for(int i = 0; i < 5; i++) { 
    int idx = s_valid_start_idx + s_scroll_offset + i;    
    if (idx >= 48) break;
    
    int y = i * 34; 
    
    if (idx == s_valid_start_idx) {
      graphics_context_set_fill_color(ctx, GColorPictonBlue);
      graphics_fill_rect(ctx, GRect(0, y, 3, 32), 0, GCornerNone);
    }

    if (s_forecast[idx].day_break == 1) {
      graphics_context_set_stroke_color(ctx, is_dark() ? GColorWhite : GColorBlack);
      graphics_draw_line(ctx, GPoint(0, y - 2), GPoint(bounds.size.w, y - 2));
      graphics_draw_line(ctx, GPoint(0, y - 1), GPoint(bounds.size.w, y - 1)); 
    } else {
      graphics_context_set_stroke_color(ctx, is_dark() ? GColorDarkGray : GColorLightGray);
      graphics_draw_line(ctx, GPoint(0, y - 2), GPoint(bounds.size.w - 12, y - 2));
    }

    if (!s_chunk_loaded[idx / 6]) {
      graphics_context_set_text_color(ctx, GColorDarkGray);
      graphics_draw_text(ctx, "Loading...", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                         GRect(x_pos[0], y + 2, 100, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      continue; 
    }

    char time_buffer[8];
    struct tm *t = localtime(&s_forecast[idx].timestamp);
    if (s_time_format == 1) { snprintf(time_buffer, sizeof(time_buffer), "%02d", t->tm_hour);
    } else {
      char ampm = (t->tm_hour >= 12) ? 'p' : 'a';
      int display_hour = t->tm_hour % 12;
      if (display_hour == 0) display_hour = 12; 
      snprintf(time_buffer, sizeof(time_buffer), "%d%c", display_hour, ampm);
    }

    if (s_forecast[idx].day_break == 1) graphics_context_set_text_color(ctx, GColorDarkCandyAppleRed);
    else graphics_context_set_text_color(ctx, is_dark() ? GColorWhite : GColorBlack);
    graphics_draw_text(ctx, time_buffer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[0], y + 2, 35, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    
    if(s_icon_bitmaps[idx]) {
      int hazard = s_forecast[idx].hazard;
      if (hazard > 0) {
        GColor haz_color = (hazard == 1) ? GColorYellow : ((hazard == 2) ? GColorOrange : GColorRed);
        graphics_context_set_fill_color(ctx, haz_color);
        graphics_fill_rect(ctx, GRect(x_pos[1] - 2, y + 1, 36, 34), 4, GCornersAll);
      }
      graphics_context_set_compositing_mode(ctx, GCompOpSet); 
      graphics_draw_bitmap_in_rect(ctx, s_icon_bitmaps[idx], GRect(x_pos[1], y + 2, 32, 32));
    }

    char temp_str[8];
    int16_t temp_val = s_forecast[idx].temp;
    snprintf(temp_str, sizeof(temp_str), "%d", temp_val);
    graphics_context_set_text_color(ctx, is_dark() ? GColorWhite : GColorBlack); 
    graphics_draw_text(ctx, temp_str, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[2], y + 2, 40, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    char wind_str[8];
    snprintf(wind_str, sizeof(wind_str), "%d", s_forecast[idx].wind);
    graphics_context_set_text_color(ctx, is_dark() ? GColorWhite : GColorBlack); 
    graphics_draw_text(ctx, wind_str, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[3], y + 2, 40, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    int rain_val = s_forecast[idx].rain;     
    GRect rain_rect = GRect(x_pos[4], y + 6, 22, 22); 
    graphics_context_set_stroke_color(ctx, is_dark() ? GColorDarkGray : GColorLightGray);
    graphics_draw_arc(ctx, rain_rect, GOvalScaleModeFitCircle, 0, TRIG_MAX_ANGLE);
    if (rain_val > 0) {
      graphics_context_set_fill_color(ctx, GColorPictonBlue);
      graphics_fill_radial(ctx, rain_rect, GOvalScaleModeFitCircle, 11, 0, (TRIG_MAX_ANGLE * rain_val) / 100); 
    }

    int uv = s_forecast[idx].uv;
    if (uv > 0) {
      GColor uv_color = GColorGreen;     
      if(uv > 2) uv_color = GColorYellow; 
      if(uv > 5) uv_color = GColorOrange; 
      if(uv > 7) uv_color = GColorRed;    
      if(uv > 10) uv_color = GColorPurple;
      
      graphics_context_set_fill_color(ctx, uv_color);
      graphics_fill_rect(ctx, GRect(bounds.size.w - 10, y + 2, 8, 30), 2, GCornersAll); 
    } else {
      graphics_context_set_fill_color(ctx, is_dark() ? GColorBlack : GColorWhite);
      graphics_fill_rect(ctx, GRect(bounds.size.w - 10, y + 2, 8, 30), 2, GCornersAll);
    }
  }

  graphics_context_set_fill_color(ctx, is_dark() ? GColorDarkGray : GColorLightGray); 
  graphics_fill_rect(ctx, GRect(bounds.size.w - 2, 0, 2, bounds.size.h), 0, GCornerNone);
  int scroll_handle_y = ((s_scroll_offset * bounds.size.h) / (48 - s_valid_start_idx));
  graphics_context_set_fill_color(ctx, is_dark() ? GColorWhite : GColorDarkGray);  
  graphics_fill_rect(ctx, GRect(bounds.size.w - 2, scroll_handle_y, 2, 30), 0, GCornerNone);
}

static void banner_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  if (s_is_error_state) graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
  else graphics_context_set_fill_color(ctx, is_dark() ? GColorDarkGray : GColorMidnightGreen);
  
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_location_name, fonts_get_system_font(FONT_KEY_GOTHIC_14), 
                     GRect(4, 0, bounds.size.w / 2, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, s_status_message, fonts_get_system_font(FONT_KEY_GOTHIC_14), 
                     GRect(bounds.size.w / 2, 0, bounds.size.w / 2 - 4, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

// ==============================================================================
// BLUETOOTH SYNC & PARSING
// ==============================================================================
static int32_t parse_next_int(char **str_ptr) {
  if (!str_ptr || !*str_ptr || **str_ptr == '\0') return 0;
  
  int32_t result = 0;
  int sign = 1;
  char *p = *str_ptr;

  if (*p == '-') {
    sign = -1;
    p++;
  }

  while (*p >= '0' && *p <= '9') {
    result = result * 10 + (*p - '0');
    p++;
  }

  while (*p != '\0' && *p != ',') { p++; }
  if (*p == ',') { p++; }

  *str_ptr = p;
  return result * sign;
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *error_tuple = dict_find(iterator, MESSAGE_KEY_ERROR_MSG);
  if (error_tuple) {
    s_is_error_state = true;
    snprintf(s_status_message, sizeof(s_status_message), "%s", error_tuple->value->cstring);
    layer_mark_dirty(s_banner_layer);
    return;
  }

  s_is_error_state = false;

  Tuple *t = dict_read_first(iterator); 
  while(t != NULL) {
    uint32_t key = t->key;
    if (key == MESSAGE_KEY_LOCATION_NAME) { 
      snprintf(s_location_name, sizeof(s_location_name), "%s", t->value->cstring); 
      for (int i = 0; i < 8; i++) s_chunk_loaded[i] = false;
    }
    else if (key == MESSAGE_KEY_TIME_FORMAT) { 
        if (strcmp(t->value->cstring, "12h") == 0) s_time_format = 0;
        else if (strcmp(t->value->cstring, "24h") == 0) s_time_format = 1;
    }
    else if (key == MESSAGE_KEY_DARK_MODE) {
        if (strcmp(t->value->cstring, "off") == 0) s_dark_mode_setting = 0;
        else if (strcmp(t->value->cstring, "on") == 0) s_dark_mode_setting = 1;
        else if (strcmp(t->value->cstring, "auto") == 0) s_dark_mode_setting = 2;
        layer_mark_dirty(s_bg_layer); 
    }
    else if (key == MESSAGE_KEY_TEMP_UNIT) {
        if (strcmp(t->value->cstring, "F") == 0) s_temp_unit = 0;
        else if (strcmp(t->value->cstring, "C") == 0) s_temp_unit = 1;
        else if (strcmp(t->value->cstring, "K") == 0) s_temp_unit = 2;
        else if (strcmp(t->value->cstring, "R") == 0) s_temp_unit = 3;
    }
    else if (key == MESSAGE_KEY_WIND_UNIT) {
        if (strcmp(t->value->cstring, "mph") == 0) s_wind_unit = 0;
        else if (strcmp(t->value->cstring, "kph") == 0) s_wind_unit = 1;
        else if (strcmp(t->value->cstring, "mps") == 0) s_wind_unit = 2;
        else if (strcmp(t->value->cstring, "knots") == 0) s_wind_unit = 3;
    }
    else if (key == MESSAGE_KEY_WEATHER_PROVIDER) {
        if (strcmp(t->value->cstring, "openmeteo") == 0) s_provider = 0;
        else if (strcmp(t->value->cstring, "nws") == 0) s_provider = 1;
        else if (strcmp(t->value->cstring, "metno") == 0) s_provider = 2;
        else if (strcmp(t->value->cstring, "openweather") == 0) s_provider = 3;
    }
    else if (key == MESSAGE_KEY_REFRESH_INTERVAL) {
      s_refresh_interval = t->value->int32;
      s_minutes_to_refresh = s_refresh_interval;
      snprintf(s_status_message, sizeof(s_status_message), "Next refresh: %dm", s_minutes_to_refresh);
    }
    t = dict_read_next(iterator); 
  }

  Tuple *chunk_idx_t = dict_find(iterator, MESSAGE_KEY_CHUNK_INDEX);
  Tuple *chunk_data_t = dict_find(iterator, MESSAGE_KEY_CHUNK_DATA);

  if (chunk_idx_t && chunk_data_t) {
    int current_chunk = chunk_idx_t->value->int32;
    char *data_str = chunk_data_t->value->cstring;
    char *p = data_str;
    
    for (int item_in_chunk = 0; item_in_chunk < 6 && *p != '\0'; item_in_chunk++) {
      char row[64];
      int i = 0;
      
      while (*p != ';' && *p != '\0' && i < (int)sizeof(row) - 1) {
        row[i++] = *p++;
      }
      row[i] = '\0';
      if (*p == ';') p++;

      int g_idx = (current_chunk * 6) + item_in_chunk;
      if (g_idx < 48) {
        char *val_ptr = row;
        
        s_forecast[g_idx].timestamp = (time_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].temp = (int16_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].wind = (int16_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].rain = (uint8_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].uv = (uint8_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].condition = (uint16_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].is_night = (uint8_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].day_break = (uint8_t)parse_next_int(&val_ptr);
        s_forecast[g_idx].hazard = (uint8_t)parse_next_int(&val_ptr);
      }
    }
    s_chunk_loaded[current_chunk] = true;
  }
  
  save_persistent_data(); 
  calculate_valid_timeline();
  update_icons_for_view();
  layer_mark_dirty(s_data_layer); 
  layer_mark_dirty(s_banner_layer);
}

// ==============================================================================
// INIT & DEINIT
// ==============================================================================
static void main_window_load(Window *window) {
  GRect bounds = layer_get_bounds(window_get_root_layer(window));
  
  s_bg_layer = layer_create(GRect(0, 0, bounds.size.w, 24));
  layer_set_update_proc(s_bg_layer, bg_update_proc);
  layer_add_child(window_get_root_layer(window), s_bg_layer);
  
  s_data_layer = layer_create(GRect(0, 24, bounds.size.w, bounds.size.h - 44));
  layer_set_update_proc(s_data_layer, data_update_proc);
  layer_add_child(window_get_root_layer(window), s_data_layer);

  s_banner_layer = layer_create(GRect(0, bounds.size.h - 20, bounds.size.w, 20));
  layer_set_update_proc(s_banner_layer, banner_update_proc);
  layer_add_child(window_get_root_layer(window), s_banner_layer);
}

static void init() {
  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) { .load = main_window_load });
  window_stack_push(s_main_window, true);
  
  for(int i = 0; i < 8; i++) { s_chunk_loaded[i] = false; }
  load_persistent_data();
  calculate_valid_timeline();

  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(8192, 512); 
  
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit() {
  tick_timer_service_unsubscribe();
  for(int i = 0; i < 48; i++) { if(s_icon_bitmaps[i]) gbitmap_destroy(s_icon_bitmaps[i]); }
  layer_destroy(s_bg_layer);
  layer_destroy(s_data_layer);
  layer_destroy(s_banner_layer);                              
  window_destroy(s_main_window);
}

int main(void) { init(); app_event_loop(); deinit(); }