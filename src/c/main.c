#include <pebble.h>

// ==============================================================================
// GLOBAL VARIABLES & STATE TRACKING
// ==============================================================================

// Core UI elements
static Window *s_main_window;       // The primary window for the app
static Layer *s_canvas_layer;       // The custom drawing layer for the grid

// Data arrays sized for a full 24-hour forecast
static char s_forecast_data[24][4][10]; // Stores text data: [Hour][Data Type][String Value]
static int s_uv_values[24];             // Stores the raw UV index integers
static int s_is_night[24];              // Boolean flag (1 or 0) for day/night icon selection
static int s_conditions[24];            // OpenWeather condition ID codes (e.g., 500 = light rain)
static GBitmap *s_icon_bitmaps[24];     // Pointers to the loaded image assets in memory
static bool s_is_metric = false;        // Tracks user preference for Celsius/kmh vs Fahrenheit/mph

// Navigation and Layout State
static int s_scroll_offset = 0;
static bool s_has_forecast = false;         // Tracks the starting hour index (0-18) for the sliding view window

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
  for(int i = 0; i < 24; i++) {
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
static void banner_timer_callback(void *data) {
  s_show_banner = false;            // Hide the banner
  s_banner_timer = NULL;            // Clear the timer reference
  layer_mark_dirty(s_canvas_layer); // Force a full UI redraw
}

// Handles scrolling up through the timeline
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scroll_offset > 0) {
    s_scroll_offset -= (s_scroll_offset == 19 ? 1 : 2);           // Shift the view window up by 2 hours
    update_icons_for_view();        // Reload memory assets for the new view
    layer_mark_dirty(s_canvas_layer); 
  }
}

// Handles scrolling down through the timeline
static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scroll_offset < 19) {       // Prevent scrolling past the 24th hour (24 total - 6 visible)
    s_scroll_offset += (s_scroll_offset == 18 ? 1 : 2);           // Shift the view window down by 2 hours
    update_icons_for_view();        // Reload memory assets for the new view
    layer_mark_dirty(s_canvas_layer);
  }
}

// Binds hardware buttons to click functions
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK && out) {
    dict_write_uint8(out, MESSAGE_KEY_REFRESH_REQUEST, 1);
    dict_write_end(out);
    app_message_outbox_send();
    snprintf(s_location_name, sizeof(s_location_name), "Refreshing weather...");
    s_show_banner = true;
    if (s_banner_timer) { app_timer_cancel(s_banner_timer); s_banner_timer = NULL; }
    layer_mark_dirty(s_canvas_layer);
  }
}
static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

// ==============================================================================
// DRAWING ENGINE (CANVAS LAYER)
// ==============================================================================

// The primary render loop called whenever the screen needs to update
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer); // Get screen dimensions

  // 1. Draw solid white background
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // 2. Define headers and column spacing
  char *temp_label = s_is_metric ? "°C" : "°F";
  char *wind_label = s_is_metric ? "km/h" : "MPH"; 
  char *headers[] = {"Time", "Cond", temp_label, wind_label, "Rain"};
  int x_pos[] = {5, 42, 85, 122, 160}; // X-coordinates for each column

  // 3. Draw Header Background Bar
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, 24), 0, GCornerNone);

  // 4. Draw Header Text
  graphics_context_set_text_color(ctx, GColorBlack);
  for(int i = 0; i < 5; i++) {
    graphics_draw_text(ctx, headers[i], fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), 
                       GRect(x_pos[i], 0, 42, 24), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  }

  // Determine how many rows to draw based on banner visibility (to prevent overlapping)
  int visible_rows = 5;
  int bottom_margin = s_show_banner ? 20 : 0; // Reserves 20px at the bottom if banner is active

  // 5. Draw the Data Grid
  for(int i = 0; i < visible_rows; i++) {
    int idx = i + s_scroll_offset;    // The actual data array index based on scroll position
    int y = 24 + (i * 34);            // The vertical pixel coordinate for the current row
    
    // Feature: Highlight the "Current Hour" (Row 0) with a blue indicator on the left
    if (idx == 0 && !s_show_banner) {
      graphics_context_set_fill_color(ctx, GColorPictonBlue);
      graphics_fill_rect(ctx, GRect(0, y, 3, 32), 0, GCornerNone);
    }

    // Draw row separator line
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_line(ctx, GPoint(0, y - 2), GPoint(bounds.size.w - 12, y - 2));

    // Draw Time Column
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, s_forecast_data[idx][0], fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[0], y + 2, 35, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    
    // Draw Condition Icon
    if(s_icon_bitmaps[idx]) {
      graphics_context_set_compositing_mode(ctx, GCompOpSet);
      graphics_draw_bitmap_in_rect(ctx, s_icon_bitmaps[idx], GRect(x_pos[1], y + 2, 32, 32));
    }

    // Draw Temperature Column (Semantic High/Low Colors)
    int temp_val = atoi(s_forecast_data[idx][1]);
    GColor temp_color = GColorBlack;
    if (!s_is_metric) { // Fahrenheit logic
      if (temp_val >= 90) temp_color = GColorRed;
      else if (temp_val <= 32) temp_color = GColorBlue;
    } else {            // Celsius logic
      if (temp_val >= 32) temp_color = GColorRed;
      else if (temp_val <= 0) temp_color = GColorBlue;
    }
    graphics_context_set_text_color(ctx, temp_color);
    graphics_draw_text(ctx, s_forecast_data[idx][1], fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[2], y + 2, 40, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    // Draw Wind Column (High Wind Warning Color)
    int wind_val = atoi(s_forecast_data[idx][2]);
    graphics_context_set_text_color(ctx, (wind_val >= (s_is_metric ? 32 : 20)) ? GColorOrange : GColorBlack);
    graphics_draw_text(ctx, s_forecast_data[idx][2], fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 
                       GRect(x_pos[3], y + 2, 40, 30), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    // Draw Rain Probability Pie Chart
    int rain_val = atoi(s_forecast_data[idx][3]); 
    GRect rain_rect = GRect(x_pos[4], y + 6, 22, 22); // Bounding box for the circle
    
    // Draw the empty background ring
    graphics_context_set_stroke_color(ctx, GColorLightGray);
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
    graphics_fill_rect(ctx, GRect(bounds.size.w - 10, y + 2, 8, 30), 2, GCornersAll); // 8px wide, 2px rounded corners
  }

  // 6. Draw Scroll Track Indicator
  graphics_context_set_fill_color(ctx, GColorLightGray); // Track background
  graphics_fill_rect(ctx, GRect(bounds.size.w - 2, 24, 2, bounds.size.h - 24 - bottom_margin), 0, GCornerNone);
  
  // Calculate relative handle position based on offset
  int scroll_handle_y = 24 + ((s_scroll_offset * (bounds.size.h - 54 - bottom_margin)) / 19);
  graphics_context_set_fill_color(ctx, GColorDarkGray);  // Moving handle
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
}

// ==============================================================================
// BLUETOOTH COMMUNICATIONS (APP MESSAGES)
// ==============================================================================

// Callback triggered whenever the phone sends a chunk of data
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *t = dict_read_first(iterator);
  bool location_received = false; // Flag to trigger the banner timer

  // Iterate through all key/value pairs in the incoming dictionary
  while(t != NULL) {
    uint32_t key = t->key;
    
    // Parse Location
    if (key == MESSAGE_KEY_LOCATION_NAME) {
      snprintf(s_location_name, sizeof(s_location_name), "Location: %s", t->value->cstring);
      location_received = true;
    } 
    // Parse Units
    else if (key == MESSAGE_KEY_UNITS) {
      s_is_metric = (strcmp(t->value->cstring, "metric") == 0);
    } 
    // Parse Array Data (Offsets applied using Message Key Math)
    else if (key >= MESSAGE_KEY_IS_NIGHT && key < MESSAGE_KEY_IS_NIGHT + 24) {
      int idx = key - MESSAGE_KEY_IS_NIGHT;
      if (s_is_night[idx] != t->value->int32) invalidate_icon(idx);
      s_is_night[idx] = t->value->int32;
    } else if (key >= MESSAGE_KEY_COLUMN_COND && key < MESSAGE_KEY_COLUMN_COND + 24) {
      int idx = key - MESSAGE_KEY_COLUMN_COND;
      if (s_conditions[idx] != (int)t->value->int32) invalidate_icon(idx);
      s_conditions[idx] = (int)t->value->int32;
    } else if (key >= MESSAGE_KEY_COLUMN_TIME && key < MESSAGE_KEY_COLUMN_TIME + 24) {
      snprintf(s_forecast_data[key - MESSAGE_KEY_COLUMN_TIME][0], 10, "%s", t->value->cstring);
    } else if (key >= MESSAGE_KEY_COLUMN_TEMP && key < MESSAGE_KEY_COLUMN_TEMP + 24) {
      snprintf(s_forecast_data[key - MESSAGE_KEY_COLUMN_TEMP][1], 10, "%s", t->value->cstring);
    } else if (key >= MESSAGE_KEY_COLUMN_WIND && key < MESSAGE_KEY_COLUMN_WIND + 24) {
      snprintf(s_forecast_data[key - MESSAGE_KEY_COLUMN_WIND][2], 10, "%s", t->value->cstring);
    } else if (key >= MESSAGE_KEY_COLUMN_RAIN && key < MESSAGE_KEY_COLUMN_RAIN + 24) {
      snprintf(s_forecast_data[key - MESSAGE_KEY_COLUMN_RAIN][3], 10, "%s", t->value->cstring);
    } else if (key >= MESSAGE_KEY_COLUMN_UV && key < MESSAGE_KEY_COLUMN_UV + 24) {
      s_uv_values[key - MESSAGE_KEY_COLUMN_UV] = (int)t->value->int32;
    }
    
    t = dict_read_next(iterator); // Move to the next key
  }
  
  // Update UI and RAM buffers after processing the payload chunk
  s_has_forecast = true;
  update_icons_for_view();

  // If the location payload arrived, begin the 3-second teardown timer
  if (location_received && s_banner_timer == NULL) {
    s_banner_timer = app_timer_register(3000, banner_timer_callback, NULL);
  }

  layer_mark_dirty(s_canvas_layer); // Redraw screen with new data
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
  
  // Register Bluetooth callbacks and open a large 8kb buffer for 24h payloads
  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(8192, 512); 
}

// App teardown
static void deinit() {
  if (s_banner_timer) app_timer_cancel(s_banner_timer); // Clear lingering timers
  for(int i = 0; i < 24; i++) { 
    if(s_icon_bitmaps[i]) gbitmap_destroy(s_icon_bitmaps[i]); // Free graphic RAM
  }
  layer_destroy(s_canvas_layer);
  window_destroy(s_main_window);
}

// Main execution loop
int main(void) { init(); app_event_loop(); deinit(); }