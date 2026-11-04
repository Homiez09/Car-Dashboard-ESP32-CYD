#include "adapter_picker.h"

#include <stdio.h>
#include <string.h>

#include "../../core/i18n.h"
#include "../../core/platform.h"
#include "../../core/settings.h"
#include "../theme.h"
#include "../ui.h"
#include "../widgets.h"
#include "dialog.h"

namespace ui::adapter_picker {
namespace {

constexpr uint8_t kMaxItems = 12;
constexpr uint32_t kRefreshMs = 400;

struct Picker {
  core::AdapterTransport transport = core::AdapterTransport::Bluetooth;
  lv_obj_t* body = nullptr;
  lv_obj_t* spinner = nullptr;
  lv_obj_t* status = nullptr;
  lv_obj_t* list = nullptr;
  lv_obj_t* again = nullptr;
  lv_timer_t* timer = nullptr;
  platform::AdapterInfo items[kMaxItems] = {};
  uint8_t count = 0;
  bool scanning = false;
};

Picker g_picker;

void format_address(const uint8_t* a, char* out, size_t len) {
  snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

const char* display_name(const platform::AdapterInfo& item, char* buf, size_t len) {
  if (item.name[0] != '\0') return item.name;
  format_address(item.address, buf, len);
  return buf;
}

bool is_current(const platform::AdapterInfo& item) {
  const core::Settings& s = core::settings::get();
  if (g_picker.transport == core::AdapterTransport::Bluetooth) {
    return s.bt_addr_valid && memcmp(s.bt_addr, item.address, sizeof(s.bt_addr)) == 0;
  }
  return strcmp(s.wifi_ssid, item.name) == 0;
}

void picked_cb(lv_event_t* e) {
  const auto index = static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (index >= g_picker.count) return;
  const platform::AdapterInfo item = g_picker.items[index];
  if (item.secured) {
    platform::play(platform::Sound::Error);
    toast(core::Severity::Warning, core::tr(core::Str::SCAN_SECURED));
    return;
  }

  char buf[24];
  core::Settings& s = core::settings::edit();
  s.demo_mode = false;
  s.adapter = g_picker.transport;
  if (g_picker.transport == core::AdapterTransport::Bluetooth) {
    memcpy(s.bt_addr, item.address, sizeof(s.bt_addr));
    s.bt_addr_valid = true;
    snprintf(s.bt_name, sizeof(s.bt_name), "%s", display_name(item, buf, sizeof(buf)));
  } else {
    snprintf(s.wifi_ssid, sizeof(s.wifi_ssid), "%s", item.name);
  }
  core::settings::commit(core::settings::kChangeDataSource);

  char text[64];
  snprintf(text, sizeof(text), core::tr(core::Str::FMT_CONNECTING_TO), display_name(item, buf, sizeof(buf)));
  dialog::close();
  platform::play(platform::Sound::Confirm);
  toast(core::Severity::Normal, text);
}

void add_row(uint8_t index) {
  const platform::AdapterInfo& item = g_picker.items[index];
  const bool current = is_current(item);

  lv_obj_t* r = card(g_picker.list);
  lv_obj_set_size(r, lv_pct(100), 38);
  lv_obj_set_style_bg_color(r, color::bg(), 0);
  lv_obj_set_style_pad_hor(r, 10, 0);
  lv_obj_set_style_pad_ver(r, 0, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 10, 0);
  lv_obj_set_clickable(r, true);
  lv_obj_set_style_bg_color(r, color::card_hi(), LV_STATE_PRESSED);
  if (current) {
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, color::accent(), 0);
  }

  const char* glyph = item.secured ? ICON_LOCK
                                   : (g_picker.transport == core::AdapterTransport::Wifi ? ICON_WIFI : ICON_BLUETOOTH);
  const lv_color_t tint = item.secured ? color::text_faint() : (item.likely_obd ? color::accent() : color::text_dim());
  icon(r, glyph, font::value(), tint);

  char buf[24];
  lv_obj_t* name = label(r, font::body(), item.secured ? color::text_dim() : color::text(),
                         display_name(item, buf, sizeof(buf)));
  lv_obj_set_flex_grow(name, 1);
  single_line(name);

  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%d dBm", item.rssi);
  label(r, font::caption(), color::text_faint(), rssi);
  if (current) icon(r, ICON_CHECK, font::value(), color::accent());

  lv_obj_add_event_cb(r, picked_cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
  add_touch_feedback(r);
}

void show_status() {
  set_visible(g_picker.spinner, g_picker.scanning);
  set_visible(g_picker.again, !g_picker.scanning);
  if (g_picker.scanning) {
    set_text(g_picker.status, core::tr(core::Str::SCAN_RUNNING));
  } else {
    set_text(g_picker.status, g_picker.count == 0 ? core::tr(core::Str::SCAN_EMPTY) : "");
  }
  set_visible(g_picker.status, g_picker.scanning || g_picker.count == 0);
}

void refresh(lv_timer_t*) {
  platform::AdapterInfo items[kMaxItems];
  const uint8_t count = platform::adapter_scan_results(items, kMaxItems);
  const bool scanning = platform::adapter_scan_running();
  const bool changed = count != g_picker.count || memcmp(items, g_picker.items, sizeof(items[0]) * count) != 0;
  if (changed) {
    memcpy(g_picker.items, items, sizeof(items[0]) * count);
    g_picker.count = count;
    lv_obj_clean(g_picker.list);
    for (uint8_t i = 0; i < count; ++i) add_row(i);
  }
  if (changed || scanning != g_picker.scanning) {
    g_picker.scanning = scanning;
    show_status();
  }
}

void start_scan() {
  platform::adapter_scan_start(g_picker.transport);
  g_picker.scanning = true;
  show_status();
}

void again_cb(lv_event_t*) { start_scan(); }

/** Forgets the selected adapter: the transport searches for one by itself. */
void auto_cb(lv_event_t*) {
  core::Settings& s = core::settings::edit();
  s.demo_mode = false;
  s.adapter = g_picker.transport;
  if (g_picker.transport == core::AdapterTransport::Bluetooth) {
    s.bt_addr_valid = false;
    s.bt_name[0] = '\0';
  } else {
    s.wifi_ssid[0] = '\0';
  }
  core::settings::commit(core::settings::kChangeDataSource);
  dialog::close();
  platform::play(platform::Sound::Confirm);
  toast(core::Severity::Normal, core::tr(core::Str::SCAN_AUTO_DONE));
}

void deleted_cb(lv_event_t* e) {
  // Dialogs are deleted asynchronously: ignore a sheet that was already replaced.
  if (lv_event_get_target_obj(e) != g_picker.body) return;
  if (g_picker.timer != nullptr) lv_timer_delete(g_picker.timer);
  g_picker = Picker{};
}

}  // namespace

void open(core::AdapterTransport transport) {
  if (g_picker.timer != nullptr) lv_timer_delete(g_picker.timer);
  g_picker = Picker{};
  g_picker.transport = transport;
  g_picker.body = dialog::sheet(core::tr(transport == core::AdapterTransport::Wifi ? core::Str::SCAN_TITLE_WIFI
                                                                                  : core::Str::SCAN_TITLE_BT));
  lv_obj_add_event_cb(g_picker.body, deleted_cb, LV_EVENT_DELETE, nullptr);

  lv_obj_t* header = row(g_picker.body, 8);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_style_pad_hor(header, 4, 0);
  g_picker.spinner = lv_spinner_create(header);
  lv_obj_set_size(g_picker.spinner, 18, 18);
  lv_spinner_set_anim_params(g_picker.spinner, 900, 240);
  lv_obj_set_style_arc_width(g_picker.spinner, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(g_picker.spinner, color::track(), LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_picker.spinner, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(g_picker.spinner, color::accent(), LV_PART_INDICATOR);
  g_picker.status = label(header, font::body(), color::text_dim(), "");
  lv_obj_set_flex_grow(g_picker.status, 1);
  lv_label_set_long_mode(g_picker.status, LV_LABEL_LONG_MODE_WRAP);

  g_picker.list = column(g_picker.body, 4);
  lv_obj_set_width(g_picker.list, lv_pct(100));

  lv_obj_t* footer = row(g_picker.body, 6);
  lv_obj_set_width(footer, lv_pct(100));
  const core::Settings& s = core::settings::get();
  const bool automatic = transport == core::AdapterTransport::Bluetooth ? !s.bt_addr_valid : s.wifi_ssid[0] == '\0';
  lv_obj_t* auto_button = button(footer, ICON_BRIGHTNESS_AUTO, core::tr(core::Str::SET_ADAPTER_AUTO),
                                 automatic ? ButtonStyle::Primary : ButtonStyle::Secondary, auto_cb, nullptr);
  lv_obj_set_flex_grow(auto_button, 1);
  g_picker.again = button(footer, ICON_SCAN, core::tr(core::Str::SCAN_AGAIN), ButtonStyle::Secondary, again_cb,
                          nullptr);
  lv_obj_set_flex_grow(g_picker.again, 1);

  g_picker.timer = lv_timer_create(refresh, kRefreshMs, nullptr);
  start_scan();
}

}  // namespace ui::adapter_picker
