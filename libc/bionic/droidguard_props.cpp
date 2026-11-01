/*
 * Copyright (C) 2026 The PixelOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "droidguard_props.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <private/android_filesystem_config.h>

// The staging props are written by SpoofManager in frameworks/base. Keep the
// names and the slot limit in sync with it.
#define PIF_STAGING_PREFIX "persist.sys.pif."
#define PIF_PRODUCT_PREFIX "persist.sys.pif.product."
#define PIF_CUSTOM_COUNT_PROP "persist.sys.pif.x.count"
#define PIF_CUSTOM_SLOT_PREFIX "persist.sys.pif.x."
#define PIF_CUSTOM_SLOT_MAX 32

#define DROIDGUARD_PROCESS_NAME "com.google.android.gms.unstable"

static bool starts_with(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool ends_with(const char* s, size_t len, const char* suffix) {
  size_t suffix_len = strlen(suffix);
  return len >= suffix_len && strcmp(s + len - suffix_len, suffix) == 0;
}

// The name is only final after Process.setArgV0(), so nothing is cached
// while the process still runs as a zygote, usap or "<pre-initialized>".
static bool read_droidguard(bool* resolved) {
  *resolved = false;

  int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char cmdline[64];
  ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
  close(fd);
  if (n <= 0) {
    return false;
  }
  cmdline[n] = '\0';

  if (starts_with(cmdline, "zygote") || starts_with(cmdline, "usap") ||
      strcmp(cmdline, "<pre-initialized>") == 0) {
    return false;
  }
  *resolved = true;
  return strcmp(cmdline, DROIDGUARD_PROCESS_NAME) == 0;
}

bool droidguard_is_current_process() {
  static _Atomic(pid_t) cached_pid = -1;
  static _Atomic(bool) cached_value = false;

  if ((getuid() % AID_USER_OFFSET) < AID_APP_START) {
    return false;
  }

  pid_t pid = getpid();
  if (atomic_load_explicit(&cached_pid, memory_order_acquire) == pid) {
    return atomic_load_explicit(&cached_value, memory_order_acquire);
  }

  bool resolved = false;
  bool result = read_droidguard(&resolved);
  if (resolved) {
    atomic_store_explicit(&cached_value, result, memory_order_release);
    atomic_store_explicit(&cached_pid, pid, memory_order_release);
  }
  return result;
}

static const char* read_staged(const char* staging_name, char* buffer) {
  if (__system_property_get(staging_name, buffer) > 0) {
    return buffer;
  }
  return nullptr;
}

// ro.product.<field> and ro.product.<partition>.<field>
static const char* product_staging(const char* name) {
  static const char* const kFields[] = {
      PIF_PRODUCT_PREFIX "manufacturer", PIF_PRODUCT_PREFIX "brand", PIF_PRODUCT_PREFIX "model",
      PIF_PRODUCT_PREFIX "device",       PIF_PRODUCT_PREFIX "name",  nullptr,
  };

  if (!starts_with(name, "ro.product.")) {
    return nullptr;
  }
  const char* field = name + strlen("ro.product.");
  const char* dot = strchr(field, '.');
  if (dot != nullptr) {
    if (dot == field) {
      return nullptr;
    }
    field = dot + 1;
    if (strchr(field, '.') != nullptr) {
      return nullptr;
    }
  }
  for (const char* const* f = kFields; *f != nullptr; ++f) {
    if (strcmp(field, *f + strlen(PIF_PRODUCT_PREFIX)) == 0) {
      return *f;
    }
  }
  return nullptr;
}

struct SuffixStaging {
  const char* suffix;
  const char* staging_name;
};

static const SuffixStaging kSuffixStaging[] = {
    {".build.id", PIF_STAGING_PREFIX "build.id"},
    {".security_patch", PIF_STAGING_PREFIX "security_patch"},
    {".real_security_patch", PIF_STAGING_PREFIX "security_patch"},
    {"api_level", PIF_STAGING_PREFIX "api_level"},
    {".build.type", PIF_STAGING_PREFIX "build.type"},
    {".build.tags", PIF_STAGING_PREFIX "build.tags"},
    {nullptr, nullptr},
};

static const char* fixed_staging(const char* name) {
  if (strcmp(name, "ro.build.product") == 0) {
    return PIF_PRODUCT_PREFIX "device";
  }
  if (strcmp(name, "ro.build.description") == 0) {
    return PIF_STAGING_PREFIX "build.description";
  }
  if (strcmp(name, "ro.build.flavor") == 0) {
    return PIF_STAGING_PREFIX "build.flavor";
  }
  if (const char* staging = product_staging(name)) {
    return staging;
  }
  if (!starts_with(name, "ro.")) {
    return nullptr;
  }
  size_t len = strlen(name);
  for (const SuffixStaging* s = kSuffixStaging; s->suffix != nullptr; ++s) {
    if (ends_with(name, len, s->suffix)) {
      return s->staging_name;
    }
  }
  return nullptr;
}

// Slot i holds "name=value", or "*suffix=value" for a suffix match. An exact
// entry wins over everything else, a suffix entry only applies last.
static const char* custom_override(const char* name, char* buffer, bool suffix) {
  char raw[PROP_VALUE_MAX];
  if (__system_property_get(PIF_CUSTOM_COUNT_PROP, raw) <= 0) {
    return nullptr;
  }
  int count = 0;
  for (const char* c = raw; *c >= '0' && *c <= '9' && count <= PIF_CUSTOM_SLOT_MAX; ++c) {
    count = count * 10 + (*c - '0');
  }
  if (count > PIF_CUSTOM_SLOT_MAX) {
    count = PIF_CUSTOM_SLOT_MAX;
  }

  size_t name_len = strlen(name);
  char slot[sizeof(PIF_CUSTOM_SLOT_PREFIX) + 2];
  memcpy(slot, PIF_CUSTOM_SLOT_PREFIX, sizeof(PIF_CUSTOM_SLOT_PREFIX) - 1);
  for (int i = 0; i < count; ++i) {
    char* p = slot + sizeof(PIF_CUSTOM_SLOT_PREFIX) - 1;
    if (i >= 10) {
      *p++ = static_cast<char>('0' + i / 10);
    }
    *p++ = static_cast<char>('0' + i % 10);
    *p = '\0';

    char entry[PROP_VALUE_MAX];
    if (__system_property_get(slot, entry) <= 0) {
      continue;
    }
    char* eq = strchr(entry, '=');
    if (eq == nullptr) {
      continue;
    }
    *eq = '\0';

    bool match;
    if (entry[0] == '*') {
      match = suffix && entry[1] != '\0' && ends_with(name, name_len, entry + 1);
    } else {
      match = !suffix && strcmp(entry, name) == 0;
    }
    if (match) {
      strcpy(buffer, eq + 1);
      return buffer;
    }
  }
  return nullptr;
}

const char* droidguard_prop_override(const char* name, char* buffer) {
  if (name == nullptr || starts_with(name, PIF_STAGING_PREFIX)) {
    return nullptr;
  }
  if (const char* value = custom_override(name, buffer, false)) {
    return value;
  }
  if (const char* staging = fixed_staging(name)) {
    if (const char* value = read_staged(staging, buffer)) {
      return value;
    }
  }
  if (strcmp(name, "init.svc.adbd") == 0) {
    return "stopped";
  }
  if (strcmp(name, "sys.usb.state") == 0) {
    return "mtp";
  }
  return custom_override(name, buffer, true);
}
