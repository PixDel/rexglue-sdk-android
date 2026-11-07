#pragma once
/**
 * @file        rex/input/touch/touch_input_driver.h
 * @brief       On-screen touch controls: a virtual controller for touch screens (Android).
 *
 * A stick that appears where the left thumb lands, A/B/X/Y, the shoulder buttons and triggers,
 * Back/Start, and two shortcut buttons that press a controller chord (settings menu, portal
 * menu). The controller counts as synthetic, so it plays as player 1 next to a real pad; real
 * pads take players 1-4 in connection order.
 *
 * touch_controls: "auto" shows them while no game controller is connected, "on" always, "off"
 * never. The eye button in the top left corner hides them (and shows them again).
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

#include <rex/input/input_driver.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window_listener.h>

namespace rex::input::touch {

class TouchInputDriver final : public InputDriver,
                               public rex::ui::WindowInputListener,
                               public rex::ui::WindowListener {
 public:
  explicit TouchInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~TouchInputDriver() override;

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnTouchEvent(rex::ui::TouchEvent& e) override;
  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;

 private:
  void DetachFromWindow();
  rex::ui::Window* attached_window_ = nullptr;
  uint32_t packet_number_ = 0;
  uint16_t last_logged_buttons_ = 0;
};

// Draws the touch controls over the picture (an ImGui dialog that stays open for the whole run).
class TouchControlsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit TouchControlsDialog(rex::ui::ImGuiDrawer* imgui_drawer);
  ~TouchControlsDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

// Whether this platform shows touch controls at all (Android).
bool TouchControlsSupported();

}  // namespace rex::input::touch
