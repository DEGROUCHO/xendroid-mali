/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef xendroid_XE_ANDROID_INPUT_DRIVER_H
#define xendroid_XE_ANDROID_INPUT_DRIVER_H

#include <array>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/hid/input_driver.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
    namespace hid {
        namespace android {

            // Guest players fed by this driver: player 1 (the on-screen pad, a keyboard and the
            // first physical controller) and three more physical controllers.
            constexpr int kMaxPads = 4;

            // Last XInputSetState of each player: left motor speed in bits 8-15, right in bits
            // 0-7 (0-255). The UI polls it and drives that player's controller vibrators.
            inline std::array<std::atomic<uint32_t>, kMaxPads> controller_rumble{};

            class AndroidInputDriver final : public InputDriver {
            public:
                struct KeyStatus{
                    ui::VirtualKey id;
                    bool pressed;
                    int value;
                };

                struct Pad {
                    std::array<KeyStatus, 24> key_status;
                    std::array<KeyStatus, 24> prev_key_status;
                    uint32_t key_status_mask = 0;
                    uint32_t packet_number = 1;
                    // Player 1 is always there; the others while their controller is.
                    bool connected = false;
                };

                // Deliberately not the global critical region: analog axes push a
                // sample per motion event and nothing here needs its semantics.
                std::mutex key_status_mutex_;

                std::array<Pad, kMaxPads> pads_;

                explicit AndroidInputDriver(xe::ui::Window* window, size_t window_z_order);
                ~AndroidInputDriver() override;

                X_STATUS Setup() override;

                X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,X_INPUT_CAPABILITIES* out_caps) override;
                X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
                X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
                X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,X_INPUT_KEYSTROKE* out_keystroke) override;

                InputType GetInputType() const override;
                // XenDroid: edge's InputSystem builds guest controller-slot
                // bindings from each driver's EnumerateDevices(). Every pad is
                // advertised, each preferring its own player slot; a pad without
                // a controller answers DEVICE_NOT_CONNECTED, which the guest sees
                // as an unplugged controller.
                std::vector<InputDeviceInfo> EnumerateDevices() override;
                void OnKey(int pad, int key_index, bool pressed, short value);
                // Players 2-4 come and go with their controllers; player 1 stays.
                void SetPadConnected(int pad, bool connected);

            private:
                bool PadConnected(uint32_t user_index);
            };
        }
    }
}

#endif //xendroid_XE_ANDROID_INPUT_DRIVER_H
