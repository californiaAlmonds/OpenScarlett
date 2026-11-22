#pragma once
// MIT DirectXTK12 GamePad subset for untouched Xbox-GDK-Samples.

namespace DirectX {

class GamePad {
public:
    static constexpr int c_MergedInput = -1;

    struct ThumbSticks {
        float leftX = 0.0f;
        float leftY = 0.0f;
        float rightX = 0.0f;
        float rightY = 0.0f;
    };

    struct State {
        ThumbSticks thumbSticks{};
        bool IsConnected() const noexcept { return false; }
        bool IsViewPressed() const noexcept { return false; }
        bool IsLeftStickPressed() const noexcept { return false; }
        bool IsLeftTriggerPressed() const noexcept { return false; }
        bool IsRightTriggerPressed() const noexcept { return false; }
    };

    class ButtonStateTracker {
    public:
        enum ButtonState {
            UP = 0,
            HELD = 1,
            RELEASED = 2,
            PRESSED = 3,
        };

        ButtonState a = UP;
        ButtonState b = UP;
        ButtonState x = UP;
        ButtonState y = UP;
        ButtonState leftStick = UP;
        ButtonState rightStick = UP;
        ButtonState leftShoulder = UP;
        ButtonState rightShoulder = UP;
        ButtonState leftTrigger = UP;
        ButtonState rightTrigger = UP;
        ButtonState back = UP;
        ButtonState view = UP;
        ButtonState start = UP;
        ButtonState menu = UP;

        void Update(const State&) noexcept {}
        void Reset() noexcept {
            a = b = x = y = leftStick = rightStick = leftShoulder = rightShoulder =
                leftTrigger = rightTrigger = back = view = start = menu = UP;
        }
    };

    State GetState(int) noexcept { return {}; }
};

}  // namespace DirectX
