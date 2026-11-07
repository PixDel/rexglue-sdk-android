/**
 * @file        input/touch/touch_input_driver.cpp
 * @brief       On-screen touch controls (see touch_input_driver.h).
 */

#include <rex/input/touch/touch_input_driver.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

#include <SDL3/SDL_gamepad.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_STRING(touch_controls, "auto", "Input",
                      "On-screen touch controls: 'auto' (while no game controller is connected), "
                      "'on' or 'off'")
    .allowed({"auto", "on", "off"});
REXCVAR_DEFINE_DOUBLE(touch_controls_opacity, 0.45, "Input",
                      "Opacity of the on-screen touch controls (0.1 - 1)")
    .range(0.1, 1.0);
REXCVAR_DEFINE_DOUBLE(touch_controls_scale, 1.0, "Input",
                      "Size of the on-screen touch controls (0.6 - 1.6)")
    .range(0.6, 1.6);
// The chords the two shortcut buttons press; the defaults match skylanders-portal's
// settings_menu_buttons (back+rb) and portal_menu_open_buttons (guide).
REXCVAR_DEFINE_STRING(touch_settings_chord, "back+rb", "Input",
                      "Buttons the touch settings button presses together");
REXCVAR_DEFINE_STRING(touch_portal_chord, "guide", "Input",
                      "Buttons the touch portal button presses together");

namespace rex::input::touch {

namespace {

constexpr DeviceId kTouchDevice = static_cast<DeviceId>(0x544F5543);  // "TOUC"
using Clock = std::chrono::steady_clock;

enum Control : int {
  kA, kB, kX, kY, kLB, kRB, kLT, kRT, kBack, kStart, kSettings, kPortal, kEye, kControlCount,
  kStick = 100,
  kNone = -1,
};

struct ControlInfo {
  const char* label;
  uint16_t button;  // X_INPUT_GAMEPAD_* (0 for triggers and the shortcut buttons)
  ImU32 color;
};

const std::array<ControlInfo, kControlCount> kControls = {{
    {"A", X_INPUT_GAMEPAD_A, IM_COL32(60, 180, 75, 255)},
    {"B", X_INPUT_GAMEPAD_B, IM_COL32(220, 60, 60, 255)},
    {"X", X_INPUT_GAMEPAD_X, IM_COL32(60, 120, 230, 255)},
    {"Y", X_INPUT_GAMEPAD_Y, IM_COL32(235, 190, 40, 255)},
    {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER, IM_COL32(200, 200, 200, 255)},
    {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER, IM_COL32(200, 200, 200, 255)},
    {"LT", 0, IM_COL32(200, 200, 200, 255)},
    {"RT", 0, IM_COL32(200, 200, 200, 255)},
    {"Back", X_INPUT_GAMEPAD_BACK, IM_COL32(200, 200, 200, 255)},
    {"Start", X_INPUT_GAMEPAD_START, IM_COL32(200, 200, 200, 255)},
    {"Menu", 0, IM_COL32(170, 140, 255, 255)},
    {"Portal", 0, IM_COL32(255, 160, 60, 255)},
    {"", 0, IM_COL32(200, 200, 200, 255)},
}};

struct Circle {
  float x = 0, y = 0, r = 0;
  bool Hit(float px, float py, float slack = 1.25f) const {
    const float dx = px - x, dy = py - y;
    return dx * dx + dy * dy <= (r * slack) * (r * slack);
  }
};

constexpr int kMaxPointers = 10;

// Everything the UI thread (touches, drawing) and the guest thread (GetDeviceState) share.
struct State {
  std::mutex mutex;
  float width = 0, height = 0;  // the picture's size in pixels, from the last draw
  std::array<Circle, kControlCount> layout{};
  std::array<uint32_t, kMaxPointers> pointer_ids{};
  std::array<int, kMaxPointers> pointer_controls{};
  // The floating stick: where the thumb went down and where it is now.
  float stick_base_x = 0, stick_base_y = 0, stick_x = 0, stick_y = 0, stick_radius = 1;
  bool stick_active = false;
  bool hidden_by_user = false;
  // Chord buttons pressed by the shortcut buttons, until this time.
  uint16_t pulse_buttons = 0;
  uint8_t pulse_lt = 0, pulse_rt = 0;
  Clock::time_point pulse_until{};

  State() {
    pointer_ids.fill(rex::ui::TouchEvent::kPointerIDNone);
    pointer_controls.fill(kNone);
  }
};

State& Shared() {
  static State state;
  return state;
}

bool GamepadConnected() {
  return SDL_HasGamepad();
}

// Whether the controls take touches and are drawn (the eye button aside).
bool ControlsVisible(const State& s) {
  const std::string mode = REXCVAR_GET(touch_controls);
  if (mode == "off" || s.hidden_by_user) return false;
  return mode == "on" || !GamepadConnected();
}

// Whether the eye button is there at all.
bool EyeVisible() {
  const std::string mode = REXCVAR_GET(touch_controls);
  if (mode == "off") return false;
  return mode == "on" || !GamepadConnected();
}

void UpdateLayout(State& s, float w, float h) {
  s.width = w;
  s.height = h;
  const float u = std::min(w, h) / 9.0f * float(REXCVAR_GET(touch_controls_scale));
  auto& l = s.layout;
  // Face buttons: a diamond in the bottom right.
  const float cx = w - 2.7f * u, cy = h - 2.6f * u, d = 1.2f * u, r = 0.62f * u;
  l[kA] = {cx, cy + d, r};
  l[kB] = {cx + d, cy, r};
  l[kX] = {cx - d, cy, r};
  l[kY] = {cx, cy - d, r};
  // Shoulders and triggers in the upper corners.
  const float sy = 2.2f * u, sr = 0.55f * u;
  l[kLT] = {1.0f * u, sy, sr};
  l[kLB] = {2.4f * u, sy, sr};
  l[kRB] = {w - 2.4f * u, sy, sr};
  l[kRT] = {w - 1.0f * u, sy, sr};
  // Back, Menu, Portal, Start along the top middle.
  const float ty = 0.8f * u, tr = 0.45f * u;
  l[kBack] = {w * 0.5f - 2.4f * u, ty, tr};
  l[kSettings] = {w * 0.5f - 0.8f * u, ty, tr};
  l[kPortal] = {w * 0.5f + 0.8f * u, ty, tr};
  l[kStart] = {w * 0.5f + 2.4f * u, ty, tr};
  l[kEye] = {0.7f * u, 0.7f * u, 0.38f * u};
  s.stick_radius = 1.3f * u;
}

void Pulse(State& s, const std::string& chord) {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  size_t start = 0;
  while (start <= chord.size()) {
    size_t plus = chord.find('+', start);
    std::string name = chord.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
    for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (name == "a") buttons |= X_INPUT_GAMEPAD_A;
    else if (name == "b") buttons |= X_INPUT_GAMEPAD_B;
    else if (name == "x") buttons |= X_INPUT_GAMEPAD_X;
    else if (name == "y") buttons |= X_INPUT_GAMEPAD_Y;
    else if (name == "lb") buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
    else if (name == "rb") buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    else if (name == "back") buttons |= X_INPUT_GAMEPAD_BACK;
    else if (name == "start") buttons |= X_INPUT_GAMEPAD_START;
    else if (name == "guide") buttons |= X_INPUT_GAMEPAD_GUIDE;
    else if (name == "up") buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    else if (name == "down") buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    else if (name == "left") buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    else if (name == "right") buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    else if (name == "ls") buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
    else if (name == "rs") buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
    else if (name == "lt") lt = 0xFF;
    else if (name == "rt") rt = 0xFF;
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  s.pulse_buttons = buttons;
  s.pulse_lt = lt;
  s.pulse_rt = rt;
  // Long enough for a game polling at 30 Hz to see the whole chord in one frame.
  s.pulse_until = Clock::now() + std::chrono::milliseconds(200);
}

int FindPointer(const State& s, uint32_t id) {
  for (int i = 0; i < kMaxPointers; ++i) {
    if (s.pointer_ids[i] == id) return i;
  }
  return -1;
}

bool ControlHeld(const State& s, int control) {
  for (int i = 0; i < kMaxPointers; ++i) {
    if (s.pointer_ids[i] != rex::ui::TouchEvent::kPointerIDNone && s.pointer_controls[i] == control) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool TouchControlsSupported() {
  return REX_PLATFORM_ANDROID != 0;
}

TouchInputDriver::TouchInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

TouchInputDriver::~TouchInputDriver() {
  DetachFromWindow();
}

X_STATUS TouchInputDriver::Setup() {
  return TouchControlsSupported() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

void TouchInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (!window) return;
  attached_window_ = window;
  // Ahead of the ImGui drawer: a touch on a control is the controller's, any other one reaches
  // the menus.
  window->AddInputListener(this, window_z_order() + 1000);
  window->AddListener(this);
}

void TouchInputDriver::OnClosing(rex::ui::UIEvent&) {
  DetachFromWindow();
}

void TouchInputDriver::DetachFromWindow() {
  rex::ui::Window* window = attached_window_;
  if (!window) return;
  attached_window_ = nullptr;
  window->RemoveInputListener(this);
  window->RemoveListener(this);
}

void TouchInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  if (!TouchControlsSupported() || REXCVAR_GET(touch_controls) == "off") return;
  DeviceInfo info;
  info.id = kTouchDevice;
  info.name = "Touch controls";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT TouchInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                 X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (id != kTouchDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kTouchDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  State& s = Shared();
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0;
  {
    std::lock_guard lock(s.mutex);
    for (int c = 0; c < kControlCount; ++c) {
      if (!ControlHeld(s, c)) continue;
      buttons |= kControls[c].button;
      if (c == kLT) lt = 0xFF;
      if (c == kRT) rt = 0xFF;
    }
    if (Clock::now() < s.pulse_until) {
      buttons |= s.pulse_buttons;
      lt = std::max(lt, s.pulse_lt);
      rt = std::max(rt, s.pulse_rt);
    }
    if (s.stick_active && s.stick_radius > 0) {
      float dx = (s.stick_x - s.stick_base_x) / s.stick_radius;
      float dy = (s.stick_y - s.stick_base_y) / s.stick_radius;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1.0f) {
        dx /= len;
        dy /= len;
      }
      lx = int16_t(std::clamp(dx * 32767.0f, -32767.0f, 32767.0f));
      ly = int16_t(std::clamp(-dy * 32767.0f, -32767.0f, 32767.0f));  // up is positive
    }
  }
  if (buttons != last_logged_buttons_) {
    REXLOG_DEBUG("Touch: buttons {:04X}", buttons);
    last_logged_buttons_ = buttons;
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = ++packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = lx;
    out_state->gamepad.thumb_ly = ly;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  (void)vibration;
  return id == kTouchDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT TouchInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                              X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  return id == kTouchDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

void TouchInputDriver::OnTouchEvent(rex::ui::TouchEvent& e) {
  State& s = Shared();
  std::lock_guard lock(s.mutex);
  if (s.width <= 0) return;  // not drawn yet
  // Touches come in physical pixels; the layout is in ImGui's logical units (as ImGuiDrawer
  // converts the mouse: medium DPI / DPI).
  float to_logical = 1.0f;
  if (rex::ui::Window* w = e.target(); w && w->GetDpi() > 0) {
    to_logical = float(w->GetMediumDpi()) / float(w->GetDpi());
  }
  const float x = e.x() * to_logical, y = e.y() * to_logical;
  const uint32_t id = e.pointer_id();
  using Action = rex::ui::TouchEvent::Action;

  if (e.action() == Action::kDown) {
    REXLOG_DEBUG("Touch: down at {:.0f},{:.0f} (picture {:.0f}x{:.0f})", x, y, s.width, s.height);
    if (EyeVisible() && s.layout[kEye].Hit(x, y, 1.5f)) {
      s.hidden_by_user = !s.hidden_by_user;
      e.set_handled(true);
      return;
    }
    if (!ControlsVisible(s)) return;
    int control = kNone;
    for (int c = 0; c < kControlCount; ++c) {
      if (c != kEye && s.layout[c].Hit(x, y)) {
        control = c;
        break;
      }
    }
    if (control == kNone && !s.stick_active && x < s.width * 0.5f && y > s.height * 0.3f) {
      control = kStick;
      s.stick_active = true;
      s.stick_base_x = s.stick_x = x;
      s.stick_base_y = s.stick_y = y;
    }
    if (control == kNone) return;  // for the menus (ImGui)
    if (control == kSettings) Pulse(s, REXCVAR_GET(touch_settings_chord));
    if (control == kPortal) Pulse(s, REXCVAR_GET(touch_portal_chord));
    const int slot = FindPointer(s, rex::ui::TouchEvent::kPointerIDNone);
    if (slot >= 0) {
      s.pointer_ids[slot] = id;
      s.pointer_controls[slot] = control;
    }
    e.set_handled(true);
    return;
  }

  const int slot = FindPointer(s, id);
  if (slot < 0) return;
  const int control = s.pointer_controls[slot];
  if (e.action() == Action::kMove) {
    if (control == kStick) {
      s.stick_x = x;
      s.stick_y = y;
    }
  } else {  // up or cancel
    if (control == kStick) s.stick_active = false;
    s.pointer_ids[slot] = rex::ui::TouchEvent::kPointerIDNone;
    s.pointer_controls[slot] = kNone;
  }
  e.set_handled(true);
}

TouchControlsDialog::TouchControlsDialog(rex::ui::ImGuiDrawer* imgui_drawer)
    : ImGuiDialog(imgui_drawer) {}

TouchControlsDialog::~TouchControlsDialog() = default;

void TouchControlsDialog::OnDraw(ImGuiIO& io) {
  if (!TouchControlsSupported()) return;
  State& s = Shared();
  std::lock_guard lock(s.mutex);
  UpdateLayout(s, io.DisplaySize.x, io.DisplaySize.y);
  if (!EyeVisible()) return;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float opacity = float(REXCVAR_GET(touch_controls_opacity));
  auto alpha = [&](ImU32 c, float a) {
    return (c & 0x00FFFFFF) | (ImU32(std::clamp(a * opacity, 0.0f, 1.0f) * 255.0f) << 24);
  };
  auto centered_text = [&](const Circle& c, const char* text, float size, ImU32 col) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text);
  };

  // The eye: shows / hides the controls.
  {
    const Circle& c = s.layout[kEye];
    const ImU32 col = alpha(IM_COL32(255, 255, 255, 255), s.hidden_by_user ? 0.8f : 1.0f);
    dl->AddCircle(ImVec2(c.x, c.y), c.r, col, 32, 2.5f);
    // An eye shape: two arcs and a pupil; struck through while the controls are hidden.
    dl->PathArcTo(ImVec2(c.x, c.y + c.r * 0.9f), c.r * 1.05f, -2.25f, -0.89f, 16);
    dl->PathStroke(col, 0, 2.0f);
    dl->PathArcTo(ImVec2(c.x, c.y - c.r * 0.9f), c.r * 1.05f, 0.89f, 2.25f, 16);
    dl->PathStroke(col, 0, 2.0f);
    dl->AddCircleFilled(ImVec2(c.x, c.y), c.r * 0.22f, col, 16);
    if (s.hidden_by_user) {
      dl->AddLine(ImVec2(c.x - c.r * 0.7f, c.y - c.r * 0.7f), ImVec2(c.x + c.r * 0.7f, c.y + c.r * 0.7f),
                  col, 2.5f);
    }
  }
  if (!ControlsVisible(s)) return;

  for (int ci = 0; ci < kControlCount; ++ci) {
    if (ci == kEye) continue;
    const Circle& c = s.layout[ci];
    const bool held = ControlHeld(s, ci);
    const ControlInfo& info = kControls[ci];
    const bool face = ci <= kY;
    const ImU32 fill = alpha(face ? info.color : IM_COL32(40, 40, 40, 255), held ? 0.95f : 0.55f);
    const ImU32 edge = alpha(IM_COL32(255, 255, 255, 255), held ? 1.0f : 0.7f);
    if (ci == kLB || ci == kRB || ci == kLT || ci == kRT) {
      const ImVec2 a(c.x - c.r * 1.2f, c.y - c.r * 0.75f), b(c.x + c.r * 1.2f, c.y + c.r * 0.75f);
      dl->AddRectFilled(a, b, fill, c.r * 0.4f);
      dl->AddRect(a, b, edge, c.r * 0.4f, 0, 2.0f);
    } else {
      dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, fill, 40);
      dl->AddCircle(ImVec2(c.x, c.y), c.r, edge, 40, 2.0f);
    }
    const float text_size = face ? c.r * 1.1f : c.r * (std::strlen(info.label) > 2 ? 0.62f : 0.85f);
    centered_text(c, info.label, text_size,
                  alpha(face ? IM_COL32(255, 255, 255, 255) : info.color, 1.0f));
  }

  // The stick: a ring where the thumb went down and the knob under it; a hint while unused.
  const float sr = s.stick_radius;
  float bx = s.stick_base_x, by = s.stick_base_y, kx = s.stick_x, ky = s.stick_y;
  if (!s.stick_active) {
    bx = kx = 2.6f * sr / 1.3f;
    by = ky = s.height - 2.6f * sr / 1.3f;
  } else {
    float dx = kx - bx, dy = ky - by;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > sr) {
      kx = bx + dx / len * sr;
      ky = by + dy / len * sr;
    }
  }
  dl->AddCircleFilled(ImVec2(bx, by), sr, alpha(IM_COL32(40, 40, 40, 255), s.stick_active ? 0.5f : 0.3f), 48);
  dl->AddCircle(ImVec2(bx, by), sr, alpha(IM_COL32(255, 255, 255, 255), 0.6f), 48, 2.0f);
  dl->AddCircleFilled(ImVec2(kx, ky), sr * 0.45f,
                      alpha(IM_COL32(220, 220, 220, 255), s.stick_active ? 0.9f : 0.5f), 32);
}

}  // namespace rex::input::touch
