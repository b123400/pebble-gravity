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
static bool repel;
static int intensity; // 0..100, 50 is the original strength
static int dot_size; // 1, 2 or 4 px
static int dot_spacing; // px between dots, from the density setting

typedef struct ClaySettings {
  GColor BackgroundColor;
  GColor DotColor;
  bool Repel;
  int Intensity;
  int DotSize;
  int DotSpacing;
} ClaySettings;
static ClaySettings settings;

// Pull of a hand on a dot, treating the hand as a uniform rod of mass from the
// center to the tip, and adding up inverse-square gravity from every point
// along it. A rod pulls hardest beside its middle, only about half as hard
// past either end, and from far away it acts like a single mass at its middle,
// so neither the tip nor the center stands out.
// Adds the pull to *pull_x/*pull_y and returns the dot's distance to the hand.
// softening keeps the pull finite for dots right on the hand.
static float hand_pull(GPoint point, GPoint midpoint,
                       float hand_unit_x, float hand_unit_y, float hand_length,
                       float softening, float *pull_x, float *pull_y) {
  float to_point_x = point.x - midpoint.x;
  float to_point_y = point.y - midpoint.y;
  float distance_along_hand = to_point_x * hand_unit_x + to_point_y * hand_unit_y;

  // From the dot to the closest point on the line through the hand
  float across_x = midpoint.x + hand_unit_x * distance_along_hand - point.x;
  float across_y = midpoint.y + hand_unit_y * distance_along_hand - point.y;
  float across_squared = across_x * across_x + across_y * across_y + softening * softening;

  // The rod runs from start to end, measured along the hand from the dot.
  // Integrating the pull over it gives these exact formulas.
  float start = -distance_along_hand;
  float end = hand_length - distance_along_hand;
  float to_start = sm_sqrt(start * start + across_squared);
  float to_end = sm_sqrt(end * end + across_squared);
  float pull_along = 1 / to_start - 1 / to_end;
  float pull_across = (end / to_end - start / to_start) / across_squared;

  *pull_x += pull_along * hand_unit_x + pull_across * across_x;
  *pull_y += pull_along * hand_unit_y + pull_across * across_y;

  // Distance to the hand itself, clamped to the rod
  if (distance_along_hand < 0) distance_along_hand = 0;
  if (distance_along_hand > hand_length) distance_along_hand = hand_length;
  float closest_x = midpoint.x + hand_unit_x * distance_along_hand - point.x;
  float closest_y = midpoint.y + hand_unit_y * distance_along_hand - point.y;
  return sm_sqrt(closest_x * closest_x + closest_y * closest_y);
}

// Cube root, used by the attract pull. A bit trick gives a first guess within a few
// percent, then two Newton steps make it accurate.
static float cube_root(float x) {
  if (x <= 0) return 0;
  union {
    float f;
    uint32_t i;
  } u;
  u.f = x;
  u.i = u.i / 3 + 709921077;
  float y = u.f;
  y = y - (y * y * y - x) / (3 * y * y);
  y = y - (y * y * y - x) / (3 * y * y);
  return y;
}

// Pushes the dot at (*x, *y) away from a hand, distorting the grid around it.
// Two effects are added together, both scaled by strength:
// - A lens: a dot at distance d from the hand moves to sqrt(d^2 + gap^2).
//   Dots right next to the hand barely spread out, so they pile up into a
//   crisp wall along the edge of the gap, which outlines the hand.
// - A long-range push that fades out slowly, so grid lines curve across the
//   whole screen. It shrinks with distance too, so it adds to the wall
//   instead of thinning it out.
// reach is the minute hand's length; the sizes below are fractions of it so
// they fit any screen.
static void hand_push(float *x, float *y, GPoint midpoint,
                      float hand_unit_x, float hand_unit_y, float hand_length,
                      float reach, float strength) {
  float gap = reach * 0.083 * strength;        // To be fine tuned
  float push = reach * 0.083 * strength;       // To be fine tuned
  float push_range = reach * 0.83;             // To be fine tuned

  // Project the dot onto the hand, clamped to the hand's segment
  float to_point_x = *x - midpoint.x;
  float to_point_y = *y - midpoint.y;
  float distance_along_hand = to_point_x * hand_unit_x + to_point_y * hand_unit_y;
  if (distance_along_hand < 0) distance_along_hand = 0;
  if (distance_along_hand > hand_length) distance_along_hand = hand_length;

  float away_x = *x - (midpoint.x + hand_unit_x * distance_along_hand);
  float away_y = *y - (midpoint.y + hand_unit_y * distance_along_hand);
  float closest_distance_to_hand = sm_sqrt(away_x * away_x + away_y * away_y);

  if (closest_distance_to_hand < 0.5) {
    // The dot is on the hand, so there's no "away" direction. Push it sideways,
    // perpendicular to the hand.
    away_x = -hand_unit_y;
    away_y = hand_unit_x;
  } else {
    away_x /= closest_distance_to_hand;
    away_y /= closest_distance_to_hand;
  }

  float d = closest_distance_to_hand;
  float new_distance = sm_sqrt(d * d + gap * gap);
  float outward = (new_distance - d)
    + push * push_range * push_range / (d * d + push_range * push_range);

  *x += away_x * outward;
  *y += away_y * outward;
}

static void bitmap_layer_update_proc(Layer *layer, GContext* ctx) {
#ifdef SCREENSHOT_HOUR
  // Fixed time for store screenshots, set by `SCREENSHOT_TIME=HH:MM pebble build`
  // (see scripts/screenshots.sh)
  int minute = SCREENSHOT_MINUTE;
  int hour = SCREENSHOT_HOUR;
#else
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int minute = (*t).tm_min;
  int hour = (*t).tm_hour;
#endif

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

  // Scales both the pull and the push. 1.0 at the default intensity of 50.
  float strength = intensity / 50.0f;
  // Attract constants, as fractions of the minute hand so they fit any screen.
  // The pull is measured in 1/pixels, so its cube root needs reach^(4/3).
  float softening = minute_hand_length * 0.07; // To be fine tuned
  float attract_scale = 0.407 * minute_hand_length * cube_root(minute_hand_length) * strength; // To be fine tuned

  // The grid reaches 100px past each edge of the screen, so attract has dots
  // to pull in and repel doesn't leave the edges empty.
  int spacing = dot_spacing < 1 ? 1 : dot_spacing;
  int row_count = (bounds.size.h + 200) / spacing;
  int col_count = (bounds.size.w + 200) / spacing;

  // Unit vectors along each hand, pointing from the center to the tip
  float minute_unit_x = sm_sin(minutes_angle_rad);
  float minute_unit_y = -sm_cos(minutes_angle_rad);
  float hour_unit_x = sm_sin(hour_angle_rad);
  float hour_unit_y = -sm_cos(hour_angle_rad);

  for (int i = 0; i < row_count; i++) {
    for (int j = 0; j < col_count; j++) {
      GPoint point = GPoint(j * spacing + spacing - 100, i * spacing + spacing - 100);

      float move_x, move_y;
      if (repel) {
        // Push away from the hour hand, then push the result away from the
        // minute hand. Adding the two pushes instead makes them fight where the
        // hands meet, leaving stray dots and a crease in the corner between
        // them. Minute goes last so its gap is always fully clear.
        float x = point.x, y = point.y;
        hand_push(&x, &y, midpoint, hour_unit_x, hour_unit_y, hour_hand_length,
                  minute_hand_length, strength);
        hand_push(&x, &y, midpoint, minute_unit_x, minute_unit_y, minute_hand_length,
                  minute_hand_length, strength);
        move_x = x - point.x;
        move_y = y - point.y;
      } else {
        // Add up the pull of both hands
        float pull_x = 0, pull_y = 0;
        float minute_distance = hand_pull(point, midpoint, minute_unit_x, minute_unit_y,
                                          minute_hand_length, softening, &pull_x, &pull_y);
        float hour_distance = hand_pull(point, midpoint, hour_unit_x, hour_unit_y,
                                        hour_hand_length, softening, &pull_x, &pull_y);
        float distance = MIN(minute_distance, hour_distance);
        float pull = sm_sqrt(pull_x * pull_x + pull_y * pull_y);

        move_x = 0;
        move_y = 0;
        if (pull > 0 && distance > 0) {
          // Far from the hands the pull fades as 1/d^2, which barely bends the
          // grid. Its cube root reaches further, so the whole screen warps.
          float move = attract_scale * cube_root(pull);
          // Smoothly limit the move to short of the hand, so dots gather into a
          // soft band along it instead of jumping past it. Closer to 1 makes
          // the band a sharper line.
          float limit = 0.85 * distance; // To be fine tuned
          float ratio = move / limit;
          move = limit * ratio / sm_sqrt(1 + ratio * ratio);
          move_x = pull_x / pull * move;
          move_y = pull_y / pull * move;
        }
      }

      // Center the dot on its moved position
      GRect dot = GRect(
        (int)round(point.x + move_x - dot_size / 2.0f),
        (int)round(point.y + move_y - dot_size / 2.0f),
        dot_size,
        dot_size);

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
  Tuple *repel_t = dict_find(iter, MESSAGE_KEY_repel);
  if(repel_t) {
    repel = repel_t->value->int32 == 1;
  }
  Tuple *intensity_t = dict_find(iter, MESSAGE_KEY_intensity);
  if(intensity_t) {
    intensity = intensity_t->value->int32;
  }
  Tuple *dot_size_t = dict_find(iter, MESSAGE_KEY_dot_size);
  if(dot_size_t) {
    // Clay sends a select's value as a string
    dot_size = dot_size_t->type == TUPLE_CSTRING
      ? atoi(dot_size_t->value->cstring)
      : dot_size_t->value->int32;
  }
  Tuple *density_t = dict_find(iter, MESSAGE_KEY_density);
  if(density_t) {
    dot_spacing = density_t->value->int32;
  }
  layer_mark_dirty(bitmap_layer);

  settings.BackgroundColor = background_color;
  settings.DotColor = dot_color;
  settings.Repel = repel;
  settings.Intensity = intensity;
  settings.DotSize = dot_size;
  settings.DotSpacing = dot_spacing;
  persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));
}

static void prv_init(void) {

  // default settings
  settings.BackgroundColor = GColorFromRGBA(205, 34, 49, 255);
  settings.DotColor = GColorWhite;
  settings.Repel = false;
  settings.Intensity = 50;
  settings.DotSize = 2;
  settings.DotSpacing = 8;

  persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));

  // apply saved data
  background_color = settings.BackgroundColor;
  dot_color = settings.DotColor;
  repel = settings.Repel;
  intensity = settings.Intensity;
  dot_size = settings.DotSize;
  dot_spacing = settings.DotSpacing;

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
