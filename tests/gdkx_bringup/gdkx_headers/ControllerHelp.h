#pragma once
#include <d3d12_xs.h>

#include <cstddef>
#include <cstdint>
#include <thread>

#include "DescriptorHeap.h"
#include "RenderTargetState.h"
#include "ResourceUploadBatch.h"
#include "SpriteBatch.h"
#include "SpriteFont.h"

// Clean-room MIT ATG ControllerHelp overlay subset for untouched Xbox-GDK-Samples.

namespace ATG {

enum class HelpID : uint32_t {
    TITLE_TEXT = 0u,
    DESCRIPTION_TEXT,
    LEFT_STICK,
    LEFT_STICK_CLICK,
    RIGHT_STICK,
    RIGHT_STICK_CLICK,
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    DPAD_ALL,
    RIGHT_SHOULDER,
    RIGHT_TRIGGER,
    LEFT_SHOULDER,
    LEFT_TRIGGER,
    A_BUTTON,
    B_BUTTON,
    X_BUTTON,
    Y_BUTTON,
    MENU_BUTTON,
    VIEW_BUTTON,
    MAX_COUNT
};

struct HelpButtonAssignment {
    HelpID id;
    const wchar_t* description;
};

class Help {
public:
    Help(
        const wchar_t*,
        const wchar_t*,
        const HelpButtonAssignment*,
        size_t,
        bool = false) noexcept {}

    Help(const Help&) = delete;
    Help& operator=(const Help&) = delete;

    void RestoreDevice(
        ID3D12Device*,
        DirectX::ResourceUploadBatch&,
        const DirectX::RenderTargetState&) noexcept {}

    void SetWindow(const RECT&) noexcept {}

    void Render(ID3D12GraphicsCommandList*) noexcept {}
};

}  // namespace ATG
