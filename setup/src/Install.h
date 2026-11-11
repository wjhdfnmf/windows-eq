#pragma once

#include "Devices.h"

#include <string>
#include <vector>

namespace weq {

struct DeviceInstallState {
    bool installed = false;      // we have a backup for this device
    std::wstring slot;           // slot we installed into
    bool active = false;         // that slot still points at our CLSID
};

DeviceInstallState deviceInstallState(const RenderDevice& dev);

int cmdInstall(const RenderDevice& dev, const std::vector<FxSlot>& slots);
int cmdUninstallDevice(const RenderDevice& dev);
int cmdUninstallAll();
int cmdStatus(const std::vector<RenderDevice>& devices);
int cmdRestartAudio();

} // namespace weq
