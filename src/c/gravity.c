#include <pebble.h>
#include <stdio.h>
#include "math.h"
#include "SmallMaths.h"

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define TWOPI 6.28318531
#define SETTINGS_KEY 1

static Window *s_window;
static Layer *bitmap_layer;

static GColor background_color;
static GColor dot_color;

typedef struct ClaySettings {
  GColor BackgroundColor;
  GColor DotColor;
} ClaySettings;
static ClaySettings settings;

// How hard a hand pulls a dot. Writes the closest point on the hand into
// closest_x/closest_y and returns the pull ratio.
// hand_unit_x/y is the unit vector from the center to the tip of the hand.
static float hand_pull(GPoint point, GPoint midpoint,
                       float hand_unit_x, float hand_unit_y, float hand_length,
                       float reach, float *closest_x, float *closest_y) {
  // Project the dot onto the hand, clamped to the hand's segment
  float to_point_x = point.x - midpoint.x;
  float to_point_y = point.y - midpoint.y;
  float distance_along_hand = to_point_x * hand_unit_x + to_point_y * hand_unit_y;
  if (distance_along_hand < 0) distance_along_hand = 0;
  if (distance_along_hand > hand_length) distance_along_hand = hand_length;

  *closest_x = midpoint.x + hand_unit_x * distance_along_hand;
  *closest_y = midpoint.y + hand_unit_y * distance_along_hand;
  float closest_distance_to_hand = sm_sqrt(
    sm_powint(point.x - *closest_x, 2) +
    sm_powint(point.y - *closest_y, 2));

  // Closer dots are pulled harder
  float distance_ratio = closest_distance_to_hand / reach; // To be fine tuned
  float moving_ratio = 0.5 / (1 + distance_ratio * distance_ratio); // To be fine tuned
  if (moving_ratio < 0) moving_ratio = 0;
  return moving_ratio;
}

static void bitmap_layer_update_proc(Layer *layer, GContext* ctx) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int minute = (*t).tm_min;
  int hour = (*t).tm_hour;

  GRect bounds = layer_get_bounds(layer);

  // background color
  graphics_context_set_fill_color(ctx, background_color);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  GPoint midpoint = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  float hour_hand_length = bounds.size.h / 4;
  GPathInfo hour_hand = {
    .num_points = 2,
    .points = (GPoint []) {GPoint(0, 0), GPoint(0, -hour_hand_length)}
  };
  GPath *path = gpath_create(&hour_hand);
  int32_t hour_angle = TRIG_MAX_ANGLE / 12 * (hour % 12) + TRIG_MAX_ANGLE / 12 * minute / 60;
  float hour_angle_rad = TWOPI / 12 * (hour % 12) + TWOPI / 12 * minute / 60;
  gpath_rotate_to(path, hour_angle);
  gpath_move_to(path, midpoint);
  // gpath_draw_outline(ctx, path);
  gpath_destroy(path);

  float minute_hand_length = MIN(bounds.size.w, bounds.size.h) / 2;
  GPathInfo minute_hand = {
    .num_points = 2,
    .points = (GPoint []) {GPoint(0, 0), GPoint(0, -minute_hand_length)}
  };
  path = gpath_create(&minute_hand);
  int32_t minutes_angle = TRIG_MAX_ANGLE / 60 * minute;
  float minutes_angle_rad = TWOPI / 60 * minute;
  gpath_rotate_to(path, minutes_angle);
  gpath_move_to(path, midpoint);
  // gpath_draw_outline(ctx, path);
  gpath_destroy(path);

  graphics_context_set_fill_color(ctx, dot_color);

  int dot_size = 2;
  int row_count = 48;
  int col_count = 48;

  // Unit vectors along each hand, pointing from the center to the tip
  float minute_unit_x = sm_sin(minutes_angle_rad);
  float minute_unit_y = -sm_cos(minutes_angle_rad);
  float hour_unit_x = sm_sin(hour_angle_rad);
  float hour_unit_y = -sm_cos(hour_angle_rad);

  for (int i = 0; i < row_count; i++) {
    for (int j = 0; j < col_count; j++) {
      GPoint point = GPoint(j * 8 + 8 - 100, i * 8 + 8 - 100);

      // Both hands use the same reach so their pull falls off the same way
      float minute_closest_x, minute_closest_y, hour_closest_x, hour_closest_y;
      float minute_ratio = hand_pull(point, midpoint, minute_unit_x, minute_unit_y,
                                     minute_hand_length, minute_hand_length,
                                     &minute_closest_x, &minute_closest_y);
      float hour_ratio = hand_pull(point, midpoint, hour_unit_x, hour_unit_y,
                                   hour_hand_length, minute_hand_length,
                                   &hour_closest_x, &hour_closest_y);

      // Add up both pulls. If they add up to more than 1 (near the center,
      // where both hands meet), scale them down so the dot lands between the
      // two targets instead of overshooting past them.
      float move_x = (minute_closest_x - point.x) * minute_ratio + (hour_closest_x - point.x) * hour_ratio;
      float move_y = (minute_closest_y - point.y) * minute_ratio + (hour_closest_y - point.y) * hour_ratio;
      float total_ratio = minute_ratio + hour_ratio;
      if (total_ratio > 1) {
        move_x /= total_ratio;
        move_y /= total_ratio;
      }

      GRect dot = GRect(
        (int)round(point.x - (dot_size-1)/2),
        (int)round(point.y - (dot_size-1)/2),
        dot_size,
        dot_size);

      dot.origin.x = dot.origin.x + move_x;
      dot.origin.y = dot.origin.y + move_y;

      graphics_fill_rect(ctx, dot, 0, GCornerNone);
    }
  }
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  bitmap_layer = layer_create(bounds);
  layer_set_update_proc(bitmap_layer, bitmap_layer_update_proc);
  layer_add_child(window_layer, bitmap_layer);
}

static void prv_window_unload(Window *window) {
  layer_destroy(bitmap_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  int minute = (*tick_time).tm_min;
  int hour = (*tick_time).tm_hour;
  layer_mark_dirty(bitmap_layer);
}

static void prv_inbox_received_handler(DictionaryIterator *iter, void *context) {
  // Read color preferences
  Tuple *bg_color_t = dict_find(iter, MESSAGE_KEY_background_color);
  if(bg_color_t) {
    background_color = GColorFromHEX(bg_color_t->value->int32);
  }
  Tuple *dot_color_t = dict_find(iter, MESSAGE_KEY_dot_color);
  if(dot_color_t) {
    dot_color = GColorFromHEX(dot_color_t->value->int32);
  }
  layer_mark_dirty(bitmap_layer);

  settings.BackgroundColor = background_color;
  settings.DotColor = dot_color;
  persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));
}

static void prv_init(void) {

  // default settings
  settings.BackgroundColor = GColorFromRGBA(205, 34, 49, 255);
  settings.DotColor = GColorWhite;

  persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));

  // apply saved data
  background_color = settings.BackgroundColor;
  dot_color = settings.DotColor;

  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_open(128, 128);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  const bool animated = true;
  window_stack_push(s_window, animated);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Done initializing, pushed window: %p", s_window);

  app_event_loop();
  prv_deinit();
}
