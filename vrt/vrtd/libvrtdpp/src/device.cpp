/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <vrtd/device.hpp>

#include <algorithm>
#include <cstdio>

namespace vrtd {

bool isNull(const DesignUuid& uuid) noexcept {
    return std::all_of(uuid.begin(), uuid.end(), [](uint8_t b) { return b == 0; });
}

std::string toString(const DesignUuid& uuid) {
    char text[37];
    std::snprintf(
        text, sizeof(text),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5],
        uuid[6], uuid[7],
        uuid[8], uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return text;
}

Device::Device(uint32_t num,
               std::string_view name,
               std::string_view bdf,
               uint16_t vendorId,
               uint16_t deviceId,
               uint16_t subsystemVendorId,
               uint16_t subsystemDeviceId,
               ShellType shellType,
               bool jtag,
               std::function<Bar(const Device&, uint8_t)> fGetBar,
               std::function<QdmaQpair(const Device&, const struct slash_qdma_qpair_add&)> fCreateQdmaQpair,
               std::function<Buffer(const Device&, BufferAllocType, uint64_t, uint64_t, BufferAllocDir, MmChannel)> fOpenBuffer,
               std::function<Buffer(const Device&, uint64_t, uint64_t, BufferAllocDir, MmChannel)> fOpenBufferRaw,
               std::function<void(const Device&, HotplugOp, uint8_t)> fHotplugOp,
               std::function<void(const Device&, ShellType)> fResetSequence,
               std::function<void(const Device&, ShellType, bool)> fSetShellState,
               std::function<DesignInfo(const Device&)> fGetDesignInfo,
               std::function<bool(const Device&, int, ShellType, const DesignUuid&, uint8_t)> fDesignWrite,
               std::function<bool(const Device&, std::string_view, ShellType, const DesignUuid&, uint8_t)> fDesignWriteFile,
               std::function<void(const Device&, int, uint8_t, uint32_t)> fCfgmemProgram,
               std::function<void(const Device&, std::string_view, uint8_t, uint32_t)> fCfgmemProgramFile,
               std::function<uint32_t(const Device&, ClockRegion)> fGetClockRate,
               std::function<uint32_t(const Device&, ClockRegion, uint32_t)> fSetClockRate,
               std::function<std::vector<SensorEntry>(const Device&)> fGetSensorInfo) {
    this->num = num;
    this->name = name;
    this->bdf = bdf;
    this->vendorId = vendorId;
    this->deviceId = deviceId;
    this->subsystemVendorId = subsystemVendorId;
    this->subsystemDeviceId = subsystemDeviceId;
    this->shellType = shellType;
    this->jtag = jtag;
    this->fGetBar = fGetBar;
    this->fCreateQdmaQpair = fCreateQdmaQpair;
    this->fOpenBuffer = fOpenBuffer;
    this->fOpenBufferRaw = fOpenBufferRaw;
    this->fHotplugOp = fHotplugOp;
    this->fResetSequence = fResetSequence;
    this->fSetShellState = fSetShellState;
    this->fGetDesignInfo = fGetDesignInfo;
    this->fDesignWrite = fDesignWrite;
    this->fDesignWriteFile = fDesignWriteFile;
    this->fCfgmemProgram = fCfgmemProgram;
    this->fCfgmemProgramFile = fCfgmemProgramFile;
    this->fGetClockRate = fGetClockRate;
    this->fSetClockRate = fSetClockRate;
    this->fGetSensorInfo = fGetSensorInfo;
}

uint32_t Device::getNum() const noexcept {
    return num;
}

const std::string& Device::getName() const noexcept {
    return name;
}

const std::string& Device::getBdf() const noexcept {
    return bdf;
}

ShellType Device::getShellType() const noexcept {
    return shellType;
}

bool Device::isJtag() const noexcept {
    return jtag;
}

uint16_t Device::getVendorId() const noexcept {
    return vendorId;
}

uint16_t Device::getDeviceId() const noexcept {
    return deviceId;
}

uint16_t Device::getSubsystemVendorId() const noexcept {
    return subsystemVendorId;
}

uint16_t Device::getSubsystemDeviceId() const noexcept {
    return subsystemDeviceId;
}

Bar Device::getBar(uint8_t num) const {
    return fGetBar(*this, num);
}

QdmaQpair Device::createQdmaQpair(const struct slash_qdma_qpair_add& cfg) const {
    return fCreateQdmaQpair(*this, cfg);
}

Buffer Device::openBuffer(BufferAllocType allocType,
                          uint64_t size,
                          uint64_t allocArg,
                          BufferAllocDir allocDir,
                          MmChannel mmChannel) const {
    return fOpenBuffer(*this, allocType, size, allocArg, allocDir, mmChannel);
}

Buffer Device::openRawBuffer(uint64_t phys_addr,
                             uint64_t size,
                             BufferAllocDir allocDir,
                             MmChannel mmChannel) const {
    return fOpenBufferRaw(*this, phys_addr, size, allocDir, mmChannel);
}

void Device::hotplugOp(HotplugOp op, uint8_t function) const {
    fHotplugOp(*this, op, function);
}

void Device::resetSequence(ShellType shellType) const {
    fResetSequence(*this, shellType);
}

void Device::setShellState(ShellType shellType, bool jtag) const {
    fSetShellState(*this, shellType, jtag);
}

DesignInfo Device::getDesignInfo() const {
    return fGetDesignInfo(*this);
}

bool Device::designWrite(int input_fd, ShellType requiredShell,
                         const DesignUuid& uuid, uint8_t flags) const {
    return fDesignWrite(*this, input_fd, requiredShell, uuid, flags);
}

bool Device::designWriteFile(std::string_view path, ShellType requiredShell,
                             const DesignUuid& uuid, uint8_t flags) const {
    return fDesignWriteFile(*this, path, requiredShell, uuid, flags);
}

void Device::cfgmemProgram(int input_fd, uint8_t bootDevice, uint32_t partition) const {
    fCfgmemProgram(*this, input_fd, bootDevice, partition);
}

void Device::cfgmemProgramFile(std::string_view path, uint8_t bootDevice, uint32_t partition) const {
    fCfgmemProgramFile(*this, path, bootDevice, partition);
}

uint32_t Device::getClockRate(ClockRegion region) const {
    return fGetClockRate(*this, region);
}

uint32_t Device::setClockRate(ClockRegion region, uint32_t rate_hz) const {
    return fSetClockRate(*this, region, rate_hz);
}

std::vector<SensorEntry> Device::getSensorInfo() const {
    return fGetSensorInfo(*this);
}

}
