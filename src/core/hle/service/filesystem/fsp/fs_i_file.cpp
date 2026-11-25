// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "common/host_stalls.h"
#include "common/storage_trace.h"
#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"

namespace Service::FileSystem {

IFile::IFile(Core::System& system_, FileSys::VirtualFile file_)
    : ServiceFramework{system_, "IFile"}, backend{std::make_unique<FileSys::Fsa::IFile>(file_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IFile::Read>, "Read"},
        {1, D<&IFile::Write>, "Write"},
        {2, D<&IFile::Flush>, "Flush"},
        {3, D<&IFile::SetSize>, "SetSize"},
        {4, D<&IFile::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
        {6, nullptr, "OperateRangeWithBuffer"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

void IFile::PulseCapture(Pulse::HandlerCapture& capture) const {
    if (!pulse_parent) throw Pulse::ServiceRejected(false, "IFile without the file system that opened it");
    // Writes the guest already saw succeed must be on disk before the checkpoint is published.
    if (!CommitForCheckpoint()) throw Pulse::ServiceRejected(true, "IFile could not commit its writes");
    capture.state.U64(capture.Reference(pulse_parent));
    capture.state.String(pulse_path);
    capture.state.U32(pulse_mode);
}

Result IFile::Read(
    FileSys::ReadOption option, Out<s64> out_size, s64 offset,
    const OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_buffer,
    s64 size) {
    LOG_DEBUG(Service_FS, "called, option={}, offset={:#x}, length={}", option.value, offset,
              size);

    // Read the data from the Storage backend
    const auto start = std::chrono::steady_clock::now();
    const u64 trace_us = Common::StorageTrace::NowUs();
    const Result result =
        backend->Read(reinterpret_cast<size_t*>(out_size.Get()), offset, out_buffer.data(), size);
    using Counter = Common::HostStalls::Counter;
    Common::HostStalls::Add(Counter::FsReads, 1);
    Common::HostStalls::Add(Counter::FsReadBytes, size > 0 ? static_cast<u64>(size) : 0);
    if (Common::StorageTrace::enabled.load(std::memory_order_relaxed)) {
        Common::StorageTrace::Record('F', Common::StorageTrace::IdOf(this), trace_us,
                                     static_cast<u64>(offset), size > 0 ? static_cast<u64>(size) : 0,
                                     Common::HostStalls::fs_request_queue_us, 0,
                                     Common::HostStalls::UsSince(start));
    }
    R_RETURN(result);
}

Result IFile::Write(
    const InBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> buffer,
    FileSys::WriteOption option, s64 offset, s64 size) {
    LOG_DEBUG(Service_FS, "called, option={}, offset={:#x}, length={}", option.value, offset,
              size);

    R_RETURN(backend->Write(offset, buffer.data(), size, option));
}

Result IFile::Flush() {
    LOG_DEBUG(Service_FS, "called");

    R_RETURN(backend->Flush());
}

Result IFile::SetSize(s64 size) {
    LOG_DEBUG(Service_FS, "called, size={}", size);

    R_RETURN(backend->SetSize(size));
}

Result IFile::GetSize(Out<s64> out_size) {
    LOG_DEBUG(Service_FS, "called");

    R_RETURN(backend->GetSize(out_size));
}

} // namespace Service::FileSystem
