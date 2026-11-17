#include "adapter_list.h"

#include <string.h>

namespace core::adapters {
namespace {

// Name fragments used by common ELM327 clones and branded adapters.
const char* const kObdNames[] = {"OBD", "ELM", "V-LINK", "VLINK", "VGATE", "ICAR", "KONNWEI", "VEEPEAK", "CARISTA"};

bool contains_upper(const char* hay, const char* needle) {
  for (const char* h = hay; *h != '\0'; ++h) {
    const char* a = h;
    const char* b = needle;
    while (*a != '\0' && *b != '\0') {
      const char c = (*a >= 'a' && *a <= 'z') ? static_cast<char>(*a - 'a' + 'A') : *a;
      if (c != *b) break;
      ++a;
      ++b;
    }
    if (*b == '\0') return true;
  }
  return false;
}

bool before(const platform::AdapterInfo& a, const platform::AdapterInfo& b) {
  if (a.likely_obd != b.likely_obd) return a.likely_obd;
  return a.rssi > b.rssi;
}

}  // namespace

bool looks_like_obd(const char* name) {
  for (const char* fragment : kObdNames) {
    if (contains_upper(name, fragment)) return true;
  }
  return false;
}

uint8_t add(platform::AdapterInfo* list, uint8_t count, uint8_t max, const platform::AdapterInfo& item,
            bool match_address) {
  for (uint8_t i = 0; i < count; ++i) {
    const bool same = match_address ? memcmp(list[i].address, item.address, sizeof(item.address)) == 0
                                    : strcmp(list[i].name, item.name) == 0;
    if (same) {
      // Keep a name we already know (Bluetooth names arrive in a later event).
      const bool keep_name = item.name[0] == '\0';
      char name[sizeof(item.name)];
      memcpy(name, list[i].name, sizeof(name));
      list[i] = item;
      if (keep_name) memcpy(list[i].name, name, sizeof(name));
      return count;
    }
  }
  if (count >= max) return count;
  list[count] = item;
  return static_cast<uint8_t>(count + 1);
}

void sort(platform::AdapterInfo* list, uint8_t count) {
  // Insertion sort: lists are short.
  for (uint8_t i = 1; i < count; ++i) {
    const platform::AdapterInfo item = list[i];
    int j = i - 1;
    while (j >= 0 && before(item, list[j])) {
      list[j + 1] = list[j];
      --j;
    }
    list[j + 1] = item;
  }
}

}  // namespace core::adapters
