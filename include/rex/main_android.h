/**
 * @file        main_android.h
 * @brief       Android helpers (API level of the running device).
 */
#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID
#include <android/api-level.h>

namespace rex {

// The API level of the device the app runs on (not the one it was built against).
inline int GetAndroidApiLevel() {
  static const int level = android_get_device_api_level();
  return level;
}

}  // namespace rex
#endif
