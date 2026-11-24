// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>
#include <winrt/Windows.Storage.h>
#include "core/file_sys/vfs/vfs_types.h"
#include "jadyx/library/game_library.h"

namespace Jadyx {
// FutureAccessList is the persisted source catalog. Unavailable tokens remain
// there so reconnecting a USB does not silently forget the user's library.
std::string RememberGameFolder(const winrt::Windows::Storage::StorageFolder& folder);
std::string RememberContentFolder(const winrt::Windows::Storage::StorageFolder& folder);
LibraryScan ScanExternalGameFolders(std::stop_token stop);
FileSys::VirtualFilesystem MakeUwpFilesystem();
struct StorageBrowserEntry { std::string path; std::wstring name; bool folder{}; std::uint64_t size{}; };
std::vector<StorageBrowserEntry> ContentFolderSources();
std::vector<StorageBrowserEntry> BrowseStorageFolder(std::string_view path, std::stop_token stop);
/// The folders in `path`, and the files `keep` accepts (by their UTF-8 name).
std::vector<StorageBrowserEntry> BrowseStorageFolder(std::string_view path, std::stop_token stop,
                                                     const std::function<bool(std::string_view)>& keep);
std::uint64_t StorageFileStamp(std::string_view path);
bool ConsumeStorageReadFailure();
/// Whether games on external storage are read through a block cache with read-ahead
/// (boot.cfg "rom_cache=0|1"; off by default: it did worse than the console's own caching).
void SetRomReadCache(bool enabled);
/// Whether that cache also reads 4 MiB ahead of in-order reads (boot.cfg "rom_readahead=0|1";
/// off by default: on a hard drive the game's own reads waited behind it).
void SetRomReadAhead(bool enabled);
/// Whether games on external storage learn which blocks follow which, and have the next ones
/// read ahead on later visits (boot.cfg "rom_learn=0|1"; on by default). Journals are kept in
/// LocalState/rom-learn.
void SetRomLearn(bool enabled);
// Bounded preflight, run on an MTA worker before starting the guest. Reports
// access/removal errors in the library instead of closing the application.
std::string CheckExternalGame(std::string_view path);
// Opt-in AppContainer gate against a sparse fixture prepared by rom-storage-gate.ps1.
bool RunRomStorageGate(bool restore, const std::function<void(std::string)>& diagnostic);
} // namespace Jadyx
