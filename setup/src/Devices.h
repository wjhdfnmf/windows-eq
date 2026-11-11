#pragma once

#include <string>
#include <vector>

namespace weq {

enum class FxSlot { SFX, MFX, EFX };

const wchar_t* slotName(FxSlot slot);
bool parseSlot(const std::wstring& s, FxSlot& out);
// "sfx,mfx" -> {SFX, MFX} (sorted, unique). False if empty or invalid.
bool parseSlots(const std::wstring& s, std::vector<FxSlot>& out);
std::wstring slotsName(const std::vector<FxSlot>& slots);   // "SFX,MFX"

// Registry value names in ...\MMDevices\Audio\Render\{guid}\FxProperties
std::wstring fxClsidValueName(FxSlot slot);     // PKEY_FX_{Stream,Mode,Endpoint}EffectClsid
std::wstring fxModesValueName(FxSlot slot);     // PKEY_{S,M,E}FX_ProcessingModes_Supported_For_Streaming
std::wstring fxAssociationValueName();          // PKEY_FX_Association
inline constexpr wchar_t kKsNodeTypeAny[] = L"{00000000-0000-0000-0000-000000000000}";

inline constexpr wchar_t kModeDefault[] = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
inline constexpr wchar_t kModeRaw[] = L"{9E90EA20-B493-4FD1-A1A8-7E1361A956CF}";

struct RenderDevice {
    int index = 0;
    std::wstring id;          // "{0.0.0.00000000}.{guid}"
    std::wstring guid;        // "{guid}" = registry key name
    std::wstring name;        // "Speakers"
    std::wstring adapter;     // "Focusrite USB Audio"
    bool isDefault = false;
    bool sysFxDisabled = false;   // "Enable audio enhancements" unticked
    bool hasFxKey = false;
    std::wstring sfx, mfx, efx;   // current CLSIDs (empty if none)
    std::wstring lfx, gfx;        // legacy pre-/post-mix CLSIDs

    std::wstring displayName() const { return name + L" (" + adapter + L")"; }
};

std::wstring renderKeyPath(const std::wstring& guid); // relative to HKLM

// Active render endpoints. Requires COM initialised on the calling thread.
std::vector<RenderDevice> listRenderDevices(std::wstring& error);

// Select by list index, GUID / endpoint id, or case-insensitive name substring.
// Empty selector = default device.
const RenderDevice* selectDevice(const std::vector<RenderDevice>& devices, const std::wstring& selector,
                                 std::wstring& error);

} // namespace weq
