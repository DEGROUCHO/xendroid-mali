/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xe_android_input_driver.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/base/logging.h"

#include <algorithm>
#include <string>

namespace xe {
    namespace hid {
        namespace android {
            AndroidInputDriver::AndroidInputDriver(xe::ui::Window* window, size_t window_z_order)
                    : InputDriver(window, window_z_order) {
                const std::array<KeyStatus, 24> keys = {{
                                         {ui::VirtualKey::kXInputPadDpadLeft,false,0},
                                         {ui::VirtualKey::kXInputPadDpadUp,false,0},
                                         {ui::VirtualKey::kXInputPadDpadRight,false,0},
                                         {ui::VirtualKey::kXInputPadDpadDown,false,0},
                                         {ui::VirtualKey::kXInputPadA,false,0},
                                         {ui::VirtualKey::kXInputPadB,false,0},
                                         {ui::VirtualKey::kXInputPadX,false,0},
                                         {ui::VirtualKey::kXInputPadY,false,0},
                                         {ui::VirtualKey::kXInputPadBack,false,0},
                                         {ui::VirtualKey::kXInputPadStart,false,0},

                                         {ui::VirtualKey::kXInputPadLShoulder,false,0},
                                         {ui::VirtualKey::kXInputPadRShoulder,false,0},
                                         {ui::VirtualKey::kXInputPadLThumbPress,false,0},
                                         {ui::VirtualKey::kXInputPadRThumbPress,false,0},
                                         {ui::VirtualKey::kXInputPadLTrigger,false,0},
                                         {ui::VirtualKey::kXInputPadRTrigger,false,0},

                                         {ui::VirtualKey::kXInputPadLThumbLeft,false,0},
                                         {ui::VirtualKey::kXInputPadLThumbUp,false,0},
                                         {ui::VirtualKey::kXInputPadLThumbRight,false,0},
                                         {ui::VirtualKey::kXInputPadLThumbDown,false,0},
                                         {ui::VirtualKey::kXInputPadRThumbLeft,false,0},
                                         {ui::VirtualKey::kXInputPadRThumbUp,false,0},
                                         {ui::VirtualKey::kXInputPadRThumbRight,false,0},
                                         {ui::VirtualKey::kXInputPadRThumbDown,false,0},
                                 }};
                for (Pad& pad : pads_) {
                    pad.key_status = keys;
                    pad.prev_key_status = keys;
                }
                pads_[0].connected = true;
            }

            AndroidInputDriver::~AndroidInputDriver() {
                for (auto& rumble : controller_rumble) rumble.store(0, std::memory_order_relaxed);
            }

            bool AndroidInputDriver::PadConnected(uint32_t user_index) {
                if (user_index >= uint32_t(kMaxPads)) return false;
                std::lock_guard<std::mutex> key_lock(key_status_mutex_);
                return pads_[user_index].connected;
            }

            X_STATUS AndroidInputDriver::Setup() { return X_STATUS_SUCCESS; }

            X_RESULT AndroidInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,X_INPUT_CAPABILITIES* out_caps) {
                //XELOGI("AID:GetCapabilities {} {}",user_index, flags);
                if (!PadConnected(user_index)) {
                    return X_ERROR_DEVICE_NOT_CONNECTED;
                }

                // TODO(benvanik): confirm with a real XInput controller.
                out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
                out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
                out_caps->flags = 0;
                out_caps->gamepad.buttons = 0xFFFF;
                out_caps->gamepad.left_trigger = 0xFF;
                out_caps->gamepad.right_trigger = 0xFF;
                out_caps->gamepad.thumb_lx = (int16_t)0xFFFFu;
                out_caps->gamepad.thumb_ly = (int16_t)0xFFFFu;
                out_caps->gamepad.thumb_rx = (int16_t)0xFFFFu;
                out_caps->gamepad.thumb_ry = (int16_t)0xFFFFu;
                out_caps->vibration.left_motor_speed = 0xFFFF;
                out_caps->vibration.right_motor_speed = 0xFFFF;
                return X_ERROR_SUCCESS;
            }

            X_RESULT AndroidInputDriver::GetState(uint32_t user_index,X_INPUT_STATE* out_state) {
                //XELOGI("AID:GetState {}",user_index);
                if (user_index >= uint32_t(kMaxPads)) {
                    return X_ERROR_DEVICE_NOT_CONNECTED;
                }

                uint16_t buttons = 0;
                uint8_t left_trigger = 0;
                uint8_t right_trigger = 0;
                int16_t thumb_lx = 0;
                int16_t thumb_ly = 0;
                int16_t thumb_rx = 0;
                int16_t thumb_ry = 0;


                // A copy taken under the lock: OnKey runs on the UI thread.
                std::array<KeyStatus, 24> keys;
                uint32_t packet_number;
                {
                    std::lock_guard<std::mutex> key_lock(key_status_mutex_);
                    Pad& pad = pads_[user_index];
                    if (!pad.connected) {
                        return X_ERROR_DEVICE_NOT_CONNECTED;
                    }
                    keys = pad.key_status;
                    packet_number = ++pad.packet_number;
                }
                if (/*window()->HasFocus() && */1) {
                    //XELOGI("AID:GetState:HasFocus");
                    for (const KeyStatus& ks : keys) {
                        // Triggers are analog: value 0-255 from a controller
                        // axis, or -1 from a digital source (touch, L2/R2 keys).
                        if (ks.id == ui::VirtualKey::kXInputPadLTrigger ||
                            ks.id == ui::VirtualKey::kXInputPadRTrigger) {
                            const uint8_t trigger =
                                !ks.pressed && ks.value <= 0 ? 0
                                : ks.value < 0 ? 0xFF
                                : uint8_t(std::min(ks.value, 0xFF));
                            if (ks.id == ui::VirtualKey::kXInputPadLTrigger) {
                                left_trigger = trigger;
                            } else {
                                right_trigger = trigger;
                            }
                            continue;
                        }
                        if (!ks.pressed) continue;
                        switch (ks.id) {
                            case ui::VirtualKey::kXInputPadA:
                                buttons |= 0x1000;  // XINPUT_GAMEPAD_A
                                break;
                            case ui::VirtualKey::kXInputPadY:
                                buttons |= 0x8000;  // XINPUT_GAMEPAD_Y
                                break;
                            case ui::VirtualKey::kXInputPadB:
                                buttons |= 0x2000;  // XINPUT_GAMEPAD_B
                                break;
                            case ui::VirtualKey::kXInputPadX:
                                buttons |= 0x4000;  // XINPUT_GAMEPAD_X
                                break;
                            case ui::VirtualKey::kXInputPadDpadLeft:
                                buttons |= 0x0004;  // XINPUT_GAMEPAD_DPAD_LEFT
                                break;
                            case ui::VirtualKey::kXInputPadDpadRight:
                                buttons |= 0x0008;  // XINPUT_GAMEPAD_DPAD_RIGHT
                                break;
                            case ui::VirtualKey::kXInputPadDpadDown:
                                buttons |= 0x0002;  // XINPUT_GAMEPAD_DPAD_DOWN
                                break;
                            case ui::VirtualKey::kXInputPadDpadUp:
                                buttons |= 0x0001;  // XINPUT_GAMEPAD_DPAD_UP
                                break;
                            case ui::VirtualKey::kXInputPadRThumbPress:
                                buttons |= 0x0080;  // XINPUT_GAMEPAD_RIGHT_THUMB
                                break;
                            case ui::VirtualKey::kXInputPadLThumbPress:
                                buttons |= 0x0040;  // XINPUT_GAMEPAD_LEFT_THUMB
                                break;
                            case ui::VirtualKey::kXInputPadBack:
                                buttons |= 0x0020;  // XINPUT_GAMEPAD_BACK
                                break;
                            case ui::VirtualKey::kXInputPadStart:
                                buttons |= 0x0010;  // XINPUT_GAMEPAD_START
                                break;
                            case ui::VirtualKey::kXInputPadLShoulder:
                                buttons |= 0x0100;  // XINPUT_GAMEPAD_LEFT_SHOULDER
                                break;
                            case ui::VirtualKey::kXInputPadRShoulder:
                                buttons |= 0x0200;  // XINPUT_GAMEPAD_RIGHT_SHOULDER
                                break;
                            case ui::VirtualKey::kXInputPadLThumbLeft:
                                thumb_lx =ks.value;//+= SHRT_MIN;
                                break;
                            case ui::VirtualKey::kXInputPadLThumbRight:
                                thumb_lx =ks.value;//+= SHRT_MAX;
                                break;
                            case ui::VirtualKey::kXInputPadLThumbDown:
                                thumb_ly =ks.value;//+= SHRT_MIN;
                                break;
                            case ui::VirtualKey::kXInputPadLThumbUp:
                                thumb_ly =ks.value;//+= SHRT_MAX;
                                break;
                            case ui::VirtualKey::kXInputPadRThumbUp:
                                thumb_ry =ks.value;//+= SHRT_MAX;
                                break;
                            case ui::VirtualKey::kXInputPadRThumbDown:
                                thumb_ry =ks.value;//+= SHRT_MIN;
                                break;
                            case ui::VirtualKey::kXInputPadRThumbRight:
                                thumb_rx =ks.value;//+= SHRT_MAX;
                                break;
                            case ui::VirtualKey::kXInputPadRThumbLeft:
                                thumb_rx =ks.value;//+= SHRT_MIN;
                                break;
                        }
                    }
                }

                out_state->packet_number = packet_number;
                out_state->gamepad.buttons = buttons;
                out_state->gamepad.left_trigger = left_trigger;
                out_state->gamepad.right_trigger = right_trigger;
                out_state->gamepad.thumb_lx = thumb_lx;
                out_state->gamepad.thumb_ly = thumb_ly;
                out_state->gamepad.thumb_rx = thumb_rx;
                out_state->gamepad.thumb_ry = thumb_ry;

                return X_ERROR_SUCCESS;
            }

            X_RESULT AndroidInputDriver::SetState(uint32_t user_index,X_INPUT_VIBRATION* vibration) {
                //XELOGI( "AID:SetState {}", user_index );
                if (!PadConnected(user_index)) {
                    return X_ERROR_DEVICE_NOT_CONNECTED;
                }
                controller_rumble[user_index].store(
                    uint32_t(uint16_t(vibration->left_motor_speed) >> 8) << 8 |
                        uint32_t(uint16_t(vibration->right_motor_speed) >> 8),
                    std::memory_order_relaxed);
                return X_ERROR_SUCCESS;
            }

            X_RESULT AndroidInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,X_INPUT_KEYSTROKE* out_keystroke) {
                //XELOGI( "AID:GetKeystroke {} {}", user_index, flags );
                if (user_index >= uint32_t(kMaxPads)) {
                    return X_ERROR_DEVICE_NOT_CONNECTED;
                }

                /*if (!is_active()) {
                    return X_ERROR_EMPTY;
                }*/

                X_RESULT result = X_ERROR_EMPTY;

                ui::VirtualKey xinput_virtual_key = ui::VirtualKey::kNone;
                uint16_t unicode = 0;
                uint16_t keystroke_flags = 0;
                uint8_t hid_code = 0;
                {
                    std::lock_guard<std::mutex> key_lock(key_status_mutex_);
                    Pad& pad = pads_[user_index];
                    if (!pad.connected) {
                        return X_ERROR_DEVICE_NOT_CONNECTED;
                    }
                    if (pad.key_status_mask==0) {
                        // No keys!
                        return X_ERROR_EMPTY;
                    }
                    // One keystroke per call, lowest index first, clearing only
                    // its bit; the guest drains the rest by polling until EMPTY.
                    for(size_t i = 0; i < pad.key_status.size(); i++){
                        if(pad.key_status_mask&(1<<i)){
                            xinput_virtual_key=pad.key_status[i].id;
                            pad.key_status_mask&=~(1<<i);
                            keystroke_flags |= pad.key_status[i].pressed
                                ? 0x0001   // XINPUT_KEYSTROKE_KEYDOWN
                                : 0x0002;  // XINPUT_KEYSTROKE_KEYUP
                            if (pad.prev_key_status[i].pressed == pad.key_status[i].pressed) {
                                keystroke_flags |= 0x0004;  // XINPUT_KEYSTROKE_REPEAT
                            }
                            result = X_ERROR_SUCCESS;
                            break;
                        }
                    }
                }

                out_keystroke->virtual_key = uint16_t(xinput_virtual_key);
                out_keystroke->unicode = unicode;
                out_keystroke->flags = keystroke_flags;
                out_keystroke->user_index = uint8_t(user_index);
                out_keystroke->hid_code = hid_code;

                // X_ERROR_EMPTY if no new keys
                // X_ERROR_DEVICE_NOT_CONNECTED if no device
                // X_ERROR_SUCCESS if key
                return result;
            }


            void AndroidInputDriver::OnKey(int pad_index, int key_index, bool pressed, short value){
                if (pad_index < 0 || pad_index >= kMaxPads || key_index < 0 || key_index >= 24) return;
                std::lock_guard<std::mutex> key_lock(key_status_mutex_);
                Pad& pad = pads_[pad_index];
                // A pad comes alive with its controller's first press, so a
                // controller already in hand when the game boots needs no replug.
                if (pressed) pad.connected = true;
                const bool was_pressed = pad.key_status[key_index].pressed;
                pad.prev_key_status[key_index] = pad.key_status[key_index];

                pad.key_status[key_index].pressed = pressed;
                pad.key_status[key_index].value = value;

                // Transitions only: analog axes re-send every motion sample, which
                // would keep GetKeystroke from ever reaching EMPTY.
                if (was_pressed != pressed) {
                    pad.key_status_mask |= (1 << key_index);
                }
            }

            void AndroidInputDriver::SetPadConnected(int pad_index, bool connected){
                if (pad_index <= 0 || pad_index >= kMaxPads) return;  // player 1 never leaves
                {
                    std::lock_guard<std::mutex> key_lock(key_status_mutex_);
                    Pad& pad = pads_[pad_index];
                    pad.connected = connected;
                    if (!connected) {
                        // Nothing stays held on a controller that is gone.
                        for (size_t i = 0; i < pad.key_status.size(); i++) {
                            pad.key_status[i].pressed = false;
                            pad.key_status[i].value = 0;
                            pad.prev_key_status[i] = pad.key_status[i];
                        }
                        pad.key_status_mask = 0;
                    }
                }
                if (!connected) controller_rumble[pad_index].store(0, std::memory_order_relaxed);
            }


            InputType AndroidInputDriver::GetInputType() const { return InputType::Controller; }

            std::vector<InputDeviceInfo> AndroidInputDriver::EnumerateDevices() {
                // XenDroid: edge's InputSystem only routes guest input to drivers
                // that are bound to a slot (slot_bindings_), and bindings are built
                // from EnumerateDevices(). Every pad is always advertised so each
                // binds to its own player slot once: pad 0 (the on-screen pad, a
                // keyboard and the first physical controller) is player 1, pads 1-3
                // players 2-4. Whether a player is plugged in is GetState's answer,
                // which InputSystem turns into the guest's devices-changed notice.
                std::vector<InputDeviceInfo> out;
                for (int i = 0; i < kMaxPads; i++) {
                    InputDeviceInfo info{};
                    info.driver_slot = uint8_t(i);
                    // Player 1 keeps its old id: saved bindings refer to it.
                    info.stable_id = i == 0 ? "android-gamepad" : "android-gamepad-" + std::to_string(i + 1);
                    info.display_name = i == 0 ? "Android Controller" : "Android Controller " + std::to_string(i + 1);
                    info.preferred_slot = int8_t(i);
                    // subtype defaults to XINPUT_DEVSUBTYPE_GAMEPAD, auto_bind to true.
                    out.push_back(std::move(info));
                }
                return out;
            }

        }
    }
}
