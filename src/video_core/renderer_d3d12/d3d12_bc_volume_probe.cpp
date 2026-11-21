// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "common/alignment.h"
#include "common/literals.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

// The BC volume probe (boot.cfg "bc_volume_probe=1"). Measurement only: nothing a game samples
// changes. On the Series a game's BC6H light volumes read black unless the CPU decodes them
// ("bc_3d=hdr"), which costs four times the memory and the decode. For each shape of BC6H 3D
// texture a game creates, the probe makes scratch textures of that shape, writes marker blocks
// into every depth slice in several ways, reads them back through the texture unit
// (d3d12_volume_probe.comp) and logs which ways read right. Two references tell a broken probe
// from a misread: a one-layer 2D texture, which games sample right, and a 2D array of as many
// layers as the volume is deep, the shape the ASTC array probe catches misread.
//
// The BC array probe (boot.cfg "bc_array_probe=1") does the same for each shape of
// block-compressed 2D array a game creates, which the emulator decodes because the Series reads
// their layers wrong: decoding them and keeping their blocks for copies (PreserveGuestBlocks) is
// most of the cost of an area load. Markers go into every layer in seven ways, among them through
// a texture of 128-bit or 64-bit integers, which the texture unit reads right, then copied or
// aliased into the compressed array.

namespace D3D12 {

using namespace TextureDetail;
using namespace Common::Literals;

namespace {

/// Markers written one by one are lone 4x4 blocks at their own 512-byte placement.
constexpr u64 MARKER_STRIDE = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
/// Three blocks a slice: first, middle, last (d3d12_array_layer_probe.comp with one level).
constexpr u32 SLOTS = 3;
/// Per channel, of the 8 bits the shader packs.
constexpr int TOLERANCE = 3;
constexpr size_t MAX_SHAPES = 6;
/// Above this many markers an array goes unprobed (the ASTC array probe's bound).
constexpr u32 MAX_ARRAY_MARKERS = 8192;
/// Left free under the app's limit while a probe's texture and data exist.
constexpr u64 HEADROOM = 384_MiB;
constexpr u32 FRAMES_BETWEEN = 30;
constexpr u32 FRAMES_WAITING_FOR_MEMORY = 120;

enum Variant : u32 {
    WholeCopy,        ///< one CopyTextureRegion of the volume, as uploads are recorded
    SliceCopies,      ///< one CopyTextureRegion a depth slice
    BlockCopies,      ///< one CopyTextureRegion a marker block
    CpuWrite,         ///< WriteToSubresource into a CPU-writable heap
    CpuHeapWholeCopy, ///< WholeCopy into a CPU-writable heap
    Flat2D,           ///< reference: a one-layer 2D texture
    Array2D,          ///< reference: a 2D array of depth layers
    VariantCount,
};

constexpr const char* VARIANT_NAMES[] = {
    "3D, one copy of the volume",
    "3D, one copy a slice",
    "3D, one copy a block",
    "3D in a CPU-writable heap, written by the CPU",
    "3D in a CPU-writable heap, one copy of the volume",
    "2D reference, one layer",
    "2D array reference",
};

enum ArrayVariant : u32 {
    ArrayBlockCopies,     ///< one CopyTextureRegion a marker block (the misread seen before)
    ArrayLayerCopies,     ///< one CopyTextureRegion a layer, as uploads are recorded
    ArrayCpuWrite,        ///< WriteToSubresource into a CPU-writable heap
    ArrayCpuHeapCopies,   ///< ArrayLayerCopies into a CPU-writable heap
    ArrayCommittedCopies, ///< ArrayLayerCopies into a committed resource
    ArrayFromUint,        ///< layers into an integer texture, then texture to texture
    ArrayAliasedUint,     ///< layers into an integer texture placed over the same memory
    ArrayVariantCount,
};

constexpr const char* ARRAY_VARIANT_NAMES[] = {
    "one copy a block",
    "one copy a layer",
    "CPU-writable heap, written by the CPU",
    "CPU-writable heap, one copy a layer",
    "committed resource, one copy a layer",
    "integer texture, then copied texture to texture",
    "integer texture over the same memory",
};

u32 Hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void PutBits(std::array<u8, 16>& block, u32 position, u32 count, u32 value) {
    for (u32 bit = 0; bit < count; ++bit) {
        if ((value >> bit) & 1) {
            block[(position + bit) / 8] |= static_cast<u8>(1U << ((position + bit) % 8));
        }
    }
}

struct Marker {
    std::array<u8, 16> block{};
    u32 expected{}; ///< packUnorm4x8 of the texel: red in the low byte
};

/// A one-color BC6H block, distinct per marker, between 1/8 and 1 per channel (fresh memory
/// reads zero). Mode 3: one region, both endpoints 10 bits as they are, every index 0.
Marker MakeMarker(u32 salt, u32 index) {
    const u32 hash = Hash(index * 0x9E3779B9U + salt);
    const std::array<u32, 3> endpoints{396 + hash % 99, 396 + (hash >> 8) % 99,
                                       396 + (hash >> 16) % 99};
    Marker marker;
    PutBits(marker.block, 0, 5, 3);
    marker.expected = 0xFFU << 24;
    for (u32 channel = 0; channel < 3; ++channel) {
        PutBits(marker.block, 5 + channel * 10, 10, endpoints[channel]);
        PutBits(marker.block, 35 + channel * 10, 10, endpoints[channel]);
        // The unsigned 10-bit endpoint unquantized, then scaled into a half float's bits.
        const u32 half = ((endpoints[channel] * 64 + 32) * 31) >> 6;
        const float value = std::ldexp(static_cast<float>(1024 + (half & 1023)),
                                       static_cast<int>(half >> 10) - 25);
        const u32 packed = static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        marker.expected |= packed << (channel * 8);
    }
    return marker;
}

/// A one-color BC1 block, between 1/2 and 1 per channel: both endpoints the same 565 color, every
/// index 0.
Marker MakeBc1Marker(u32 salt, u32 index) {
    const u32 hash = Hash(index * 0x9E3779B9U + salt);
    const u32 red = 16 + hash % 16;
    const u32 green = 32 + (hash >> 8) % 32;
    const u32 blue = 16 + (hash >> 16) % 16;
    const u32 color = (red << 11) | (green << 5) | blue;
    Marker marker;
    for (u32 endpoint = 0; endpoint < 2; ++endpoint) {
        marker.block[endpoint * 2] = static_cast<u8>(color & 0xFF);
        marker.block[endpoint * 2 + 1] = static_cast<u8>(color >> 8);
    }
    marker.expected = ((red << 3) | (red >> 2)) | (((green << 2) | (green >> 4)) << 8) |
                      (((blue << 3) | (blue >> 2)) << 16) | (0xFFU << 24);
    return marker;
}

/// BC6H markers for 16-byte blocks, BC1 for 8-byte ones.
Marker MakeMarkerFor(u32 block_bytes, u32 salt, u32 index) {
    return block_bytes == 8 ? MakeBc1Marker(salt, index) : MakeMarker(salt, index);
}

bool Matches(u32 read, u32 expected) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        const int a = static_cast<int>((read >> shift) & 0xFF);
        const int b = static_cast<int>((expected >> shift) & 0xFF);
        if (std::abs(a - b) > TOLERANCE) {
            return false;
        }
    }
    return true;
}

/// Texel origin of marker `slot` of a slice, as the shader finds it.
std::pair<u32, u32> PlaceOf(u32 slot, u32 width, u32 height) {
    const u32 blocks_x = width / 4;
    const u32 blocks_y = height / 4;
    if (slot == 1) {
        return {(blocks_x / 2) * 4, (blocks_y / 2) * 4};
    }
    if (slot == 2) {
        return {(blocks_x - 1) * 4, (blocks_y - 1) * 4};
    }
    return {0, 0};
}

} // Anonymous namespace

void SetBcVolumeProbe(bool enabled) {
    probe_bc_volumes.store(enabled, std::memory_order_relaxed);
}

void TextureCacheRuntime::RequestBcVolumeProbe(const VideoCommon::ImageInfo& image_info) {
    if (!probe_bc_volumes.load(std::memory_order_relaxed)) {
        return;
    }
    const u32 probe_width = image_info.size.width;
    const u32 probe_height = image_info.size.height;
    const u32 probe_depth = image_info.size.depth;
    if (probe_width % 4 != 0 || probe_height % 4 != 0 || probe_width < 8 || probe_height < 8 ||
        probe_depth < 2 || probe_depth > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        image_info.resources.levels != 1 || bc_volume_shapes.size() >= MAX_SHAPES) {
        return;
    }
    for (const BcVolumeShape& known : bc_volume_shapes) {
        if (known.width == probe_width && known.height == probe_height &&
            known.depth == probe_depth) {
            return;
        }
    }
    bc_volume_shapes.push_back({.width = probe_width, .height = probe_height,
                                .depth = probe_depth});
    LOG_INFO(Render, "D3D12: BC volume probe queued for {}x{}x{} BC6H", probe_width,
             probe_height, probe_depth);
}

void SetBcArrayProbe(bool enabled) {
    probe_bc_arrays.store(enabled, std::memory_order_relaxed);
}

void TextureCacheRuntime::RequestBcArrayProbe(const VideoCommon::ImageInfo& image_info) {
    if (!probe_bc_arrays.load(std::memory_order_relaxed)) {
        return;
    }
    const u32 probe_width = image_info.size.width;
    const u32 probe_height = image_info.size.height;
    const u32 probe_layers = static_cast<u32>(image_info.resources.layers);
    const u32 block_bytes = VideoCore::Surface::BytesPerBlock(image_info.format);
    if ((block_bytes != 8 && block_bytes != 16) || probe_width % 4 != 0 ||
        probe_height % 4 != 0 || probe_width < 8 || probe_height < 8 || probe_layers < 2 ||
        probe_layers * SLOTS > MAX_ARRAY_MARKERS ||
        probe_layers > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        bc_volume_shapes.size() >= MAX_SHAPES) {
        return;
    }
    for (const BcVolumeShape& known : bc_volume_shapes) {
        if (known.array && known.width == probe_width && known.height == probe_height &&
            known.depth == probe_layers && known.block_bytes == block_bytes) {
            return;
        }
    }
    bc_volume_shapes.push_back({.width = probe_width, .height = probe_height,
                                .depth = probe_layers, .array = true,
                                .block_bytes = block_bytes});
    LOG_INFO(Render, "D3D12: BC array probe queued for {}x{}x{} ({}, {}-byte blocks)",
             probe_width, probe_height, probe_layers, image_info.format, block_bytes);
}

void TextureCacheRuntime::TickBcVolumeProbes() {
    if (bc_volume_probe) {
        if (!scheduler.IsFree(bc_volume_probe->tick)) {
            return;
        }
        FinishBcVolumeProbe();
        bc_volume_probe.reset();
        bc_volume_probe_wait = FRAMES_BETWEEN;
        return;
    }
    if (bc_volume_shapes.empty()) {
        return;
    }
    if (bc_volume_probe_wait != 0) {
        --bc_volume_probe_wait;
        return;
    }
    for (BcVolumeShape& shape : bc_volume_shapes) {
        const u32 variant_count =
            shape.array ? static_cast<u32>(ArrayVariantCount) : static_cast<u32>(VariantCount);
        if (shape.next_variant >= variant_count) {
            continue;
        }
        const char* const kind = shape.array ? "array" : "volume";
        if (!blit_helper || !blit_helper->CanProbeArrayLayers() ||
            (!shape.array && !blit_helper->CanProbeVolumes())) {
            LOG_WARNING(Render, "D3D12: BC {} probe {}x{}x{}: no probe shader, not run", kind,
                        shape.width, shape.height, shape.depth);
            shape.next_variant = variant_count;
            continue;
        }
        // The data, the texture and, for arrays, the integer texture.
        const u64 shape_bytes =
            u64{shape.width / 4} * (shape.height / 4) * shape.block_bytes * shape.depth;
        const u64 wanted = shape_bytes * (shape.array ? 3 : 2) + HEADROOM;
        if (pressure_sampled && pressure_snapshot.app_limit != 0 &&
            pressure_snapshot.AppFree() < wanted) {
            if (!std::exchange(shape.waited, true)) {
                LOG_INFO(Render, "D3D12: BC {} probe {}x{}x{} waits for memory ({} MiB free, "
                         "{} MiB wanted)", kind, shape.width, shape.height, shape.depth,
                         pressure_snapshot.AppFree() >> 20, wanted >> 20);
            }
            bc_volume_probe_wait = FRAMES_WAITING_FOR_MEMORY;
            return;
        }
        const u32 variant = shape.next_variant++;
        try {
            if (shape.array) {
                StartBcArrayProbe(shape, variant);
            } else {
                StartBcVolumeProbe(shape, variant);
            }
        } catch (const std::exception& exception) {
            bc_volume_probe.reset();
            LOG_WARNING(Render, "D3D12: BC {} probe {}x{}x{} [{}] not run: {}", kind,
                        shape.width, shape.height, shape.depth,
                        shape.array ? ARRAY_VARIANT_NAMES[variant] : VARIANT_NAMES[variant],
                        exception.what());
        }
        if (!bc_volume_probe) {
            bc_volume_probe_wait = 1;
        }
        return;
    }
}

void TextureCacheRuntime::StartBcVolumeProbe(const BcVolumeShape& shape, u32 variant) {
    const bool is_volume = variant != Flat2D && variant != Array2D;
    const bool cpu_heap = variant == CpuWrite || variant == CpuHeapWholeCopy;
    const bool whole_data = variant == WholeCopy || variant == CpuHeapWholeCopy ||
                            variant == SliceCopies;
    const u32 slices = variant == Flat2D ? 1 : shape.depth;
    const u32 total = slices * SLOTS;
    const u32 row_pitch = Common::AlignUp(shape.width / 4 * 16,
                                          static_cast<u32>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const u64 slice_bytes = u64{row_pitch} * (shape.height / 4);
    const auto skip = [&](const char* why) {
        LOG_INFO(Render, "D3D12: BC volume probe {}x{}x{} [{}] not run: {}", shape.width,
                 shape.height, shape.depth, VARIANT_NAMES[variant], why);
    };
    if (variant == SliceCopies && slice_bytes % MARKER_STRIDE != 0) {
        skip("a slice is not a whole number of placements");
        return;
    }
    const D3D12_RESOURCE_DESC desc{
        .Dimension = is_volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0, .Width = shape.width, .Height = shape.height,
        .DepthOrArraySize = static_cast<u16>(slices), .MipLevels = 1,
        .Format = DXGI_FORMAT_BC6H_TYPELESS, .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    ID3D12Device* const dev = device.Get();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed_footprint{};
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &placed_footprint, nullptr, nullptr, nullptr);
    const DXGI_FORMAT copy_format = placed_footprint.Footprint.Format;

    BcVolumeProbe probe{};
    probe.width = shape.width;
    probe.height = shape.height;
    probe.slices = slices;
    probe.variant = variant;
    probe.salt = Hash(++bc_volume_probe_count * 0x85EBCA6BU);
    probe.texture = texture_allocator.Create(desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             {.bytes = 16, .width = 4, .height = 4}, cpu_heap);
    ID3D12Resource* const texture = probe.texture.resource.Get();
    if (!texture) {
        skip("the texture could not be created");
        return;
    }
    const auto release_texture = [&] { texture_allocator.DeferRelease(std::move(probe.texture)); };
    if (cpu_heap && !probe.texture.cpu_writable) {
        skip("textures do not get a CPU-writable heap here");
        release_texture();
        return;
    }
    if (variant != CpuWrite) {
        HRESULT data_hr = S_OK;
        probe.markers = CreateCommittedBuffer(
            dev, whole_data ? slice_bytes * slices : MARKER_STRIDE * total, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "BC volume probe data",
            &data_hr);
        if (!probe.markers) {
            skip("no memory for the data");
            release_texture();
            return;
        }
        u8* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        ThrowIfFailed(probe.markers->Map(0, &no_read, reinterpret_cast<void**>(&mapped)),
                      "Map BC volume probe data");
        for (u32 index = 0; index < total; ++index) {
            const Marker marker = MakeMarker(probe.salt, index);
            const auto [x, y] = PlaceOf(index % SLOTS, shape.width, shape.height);
            const u64 offset = whole_data ? (index / SLOTS) * slice_bytes +
                                                u64{y / 4} * row_pitch + u64{x / 4} * 16
                                          : MARKER_STRIDE * index;
            std::memcpy(mapped + offset, marker.block.data(), marker.block.size());
        }
        probe.markers->Unmap(0, nullptr);
    }
    const u64 result_bytes = Common::AlignUp(total * 4ULL, 256ULL);
    probe.results = CreateTransferBuffer(dev, result_bytes, true);
    probe.readback = CreateCommittedBuffer(dev, result_bytes, D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_FLAG_NONE, "BC volume probe readback");

    auto* const commands = scheduler.CommandList();
    if (variant == CpuWrite) {
        // Map with no pointer keeps a texture of unknown layout mapped for the write.
        HRESULT write_hr = texture->Map(0, nullptr, nullptr);
        if (SUCCEEDED(write_hr)) {
            for (u32 index = 0; index < total && SUCCEEDED(write_hr); ++index) {
                const Marker marker = MakeMarker(probe.salt, index);
                const auto [x, y] = PlaceOf(index % SLOTS, shape.width, shape.height);
                const u32 z = index / SLOTS;
                const D3D12_BOX box{x, y, z, x + 4, y + 4, z + 1};
                write_hr = texture->WriteToSubresource(0, &box, marker.block.data(), 16, 16);
            }
            texture->Unmap(0, nullptr);
        }
        if (FAILED(write_hr)) {
            LOG_INFO(Render, "D3D12: BC volume probe {}x{}x{} [{}] not run: the write was "
                     "refused ({:#010x})", shape.width, shape.height, shape.depth,
                     VARIANT_NAMES[variant], static_cast<u32>(write_hr));
            release_texture();
            return;
        }
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{.pResource = probe.markers.Get(),
                                        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        D3D12_TEXTURE_COPY_LOCATION dst{.pResource = texture,
                                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = 0;
        if (variant == WholeCopy || variant == CpuHeapWholeCopy) {
            src.PlacedFootprint = {.Offset = 0,
                                   .Footprint = {.Format = copy_format, .Width = shape.width,
                                                 .Height = shape.height, .Depth = slices,
                                                 .RowPitch = row_pitch}};
            commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        } else if (variant == SliceCopies) {
            for (u32 z = 0; z < slices; ++z) {
                src.PlacedFootprint = {.Offset = z * slice_bytes,
                                       .Footprint = {.Format = copy_format,
                                                     .Width = shape.width,
                                                     .Height = shape.height, .Depth = 1,
                                                     .RowPitch = row_pitch}};
                commands->CopyTextureRegion(&dst, 0, 0, z, &src, nullptr);
            }
        } else {
            for (u32 index = 0; index < total; ++index) {
                const auto [x, y] = PlaceOf(index % SLOTS, shape.width, shape.height);
                const u32 slice = index / SLOTS;
                src.PlacedFootprint = {.Offset = MARKER_STRIDE * index,
                                       .Footprint = {.Format = copy_format, .Width = 4,
                                                     .Height = 4, .Depth = 1,
                                                     .RowPitch =
                                                         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
                dst.SubresourceIndex = variant == Array2D ? slice : 0;
                commands->CopyTextureRegion(&dst, x, y, is_volume ? slice : 0, &src, nullptr);
            }
        }
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    TransitionBuffer(commands, probe.results.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv = view_descriptors.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_BC6H_UF16;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (is_volume) {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv_desc.Texture3D = {.MostDetailedMip = 0, .MipLevels = 1, .ResourceMinLODClamp = 0.0f};
    } else {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv_desc.Texture2DArray = {.MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0,
                                   .ArraySize = slices, .PlaneSlice = 0,
                                   .ResourceMinLODClamp = 0.0f};
    }
    dev->CreateShaderResourceView(texture, &srv_desc, srv);
    blit_helper->ProbeArrayLayers({.source = srv,
                                   .destination = probe.results->GetGPUVirtualAddress(),
                                   .width = shape.width,
                                   .height = shape.height,
                                   .layers = slices,
                                   .levels = 1,
                                   .volume = is_volume});
    view_descriptors.Free(srv);
    TransitionBuffer(commands, probe.results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(probe.readback.Get(), 0, probe.results.Get(), 0, total * 4ULL);
    probe.tick = scheduler.CurrentTick();
    bc_volume_probe = std::move(probe);
}

void TextureCacheRuntime::FinishBcVolumeProbe() {
    BcVolumeProbe& probe = *bc_volume_probe;
    const u32 total = probe.slices * SLOTS;
    const u32* words = nullptr;
    const D3D12_RANGE read_range{0, total * 4ULL};
    if (FAILED(probe.readback->Map(0, &read_range,
                                   reinterpret_cast<void**>(const_cast<u32**>(&words))))) {
        LOG_WARNING(Render, "D3D12: BC probe readback failed to map");
        if (probe.texture.resource) {
            texture_allocator.DeferRelease(std::move(probe.texture));
        }
        return;
    }
    std::vector<u32> bad_slices;
    u32 bad_markers = 0;
    u32 black_markers = 0;
    u32 first_read = 0;
    u32 first_expected = 0;
    for (u32 index = 0; index < total; ++index) {
        const u32 expected = MakeMarkerFor(probe.block_bytes, probe.salt, index).expected;
        if (Matches(words[index], expected)) {
            continue;
        }
        if (bad_markers++ == 0) {
            first_read = words[index];
            first_expected = expected;
        }
        black_markers += (words[index] & 0xFFFFFF) == 0 ? 1 : 0;
        if (bad_slices.empty() || bad_slices.back() != index / SLOTS) {
            bad_slices.push_back(index / SLOTS);
        }
    }
    const D3D12_RANGE no_write{0, 0};
    probe.readback->Unmap(0, &no_write);
    if (probe.texture.resource) {
        texture_allocator.DeferRelease(std::move(probe.texture));
    }
    const char* const kind = probe.array ? "array" : "volume";
    const char* const name =
        probe.array ? ARRAY_VARIANT_NAMES[probe.variant] : VARIANT_NAMES[probe.variant];

    if (bad_markers == 0) {
        LOG_INFO(Render, "D3D12: BC {} probe {}x{}x{} ({}-byte blocks) [{}]: all {} markers read "
                 "right", kind, probe.width, probe.height, probe.slices, probe.block_bytes, name,
                 total);
        return;
    }
    // Which layers read right: a misread of every layer but the multiples of 4 is a layout
    // that groups layers by four.
    u32 right_fourth = 0;
    u32 right_other = 0;
    for (u32 layer = 0, next_bad = 0; layer < probe.slices; ++layer) {
        if (next_bad < bad_slices.size() && bad_slices[next_bad] == layer) {
            ++next_bad;
            continue;
        }
        ++(layer % 4 == 0 ? right_fourth : right_other);
    }
    std::string list;
    for (size_t i = 0; i < bad_slices.size() && i < 24; ++i) {
        list += fmt::format("{}{}", i ? "," : "", bad_slices[i]);
    }
    if (bad_slices.size() > 24) {
        list += ",...";
    }
    LOG_WARNING(Render, "D3D12: BC {} probe {}x{}x{} ({}-byte blocks) [{}]: {} of {} markers "
                "misread, {} of them black, in {} of {} slices [{}] (first read 0x{:08X}, wanted "
                "0x{:08X}); right: {} slices multiple of 4, {} others",
                kind, probe.width, probe.height, probe.slices, probe.block_bytes, name,
                bad_markers, total, black_markers, bad_slices.size(), probe.slices, list,
                first_read, first_expected, right_fourth, right_other);
}

void TextureCacheRuntime::StartBcArrayProbe(const BcVolumeShape& shape, u32 variant) {
    const u32 layers = shape.depth;
    const u32 total = layers * SLOTS;
    const u32 block_bytes = shape.block_bytes;
    const bool wide = block_bytes == 16;
    const bool through_uint = variant == ArrayFromUint || variant == ArrayAliasedUint;
    const bool cpu_heap = variant == ArrayCpuWrite || variant == ArrayCpuHeapCopies;
    const auto skip = [&](const char* why) {
        LOG_INFO(Render, "D3D12: BC array probe {}x{}x{} ({}-byte blocks) [{}] not run: {}",
                 shape.width, shape.height, layers, block_bytes, ARRAY_VARIANT_NAMES[variant],
                 why);
    };
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0, .Width = shape.width, .Height = shape.height,
        .DepthOrArraySize = static_cast<u16>(layers), .MipLevels = 1,
        .Format = wide ? DXGI_FORMAT_BC6H_TYPELESS : DXGI_FORMAT_BC1_TYPELESS,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    // One texel a block: the same bytes in the same rows as the compressed layer.
    D3D12_RESOURCE_DESC uint_desc = desc;
    uint_desc.Width = shape.width / 4;
    uint_desc.Height = shape.height / 4;
    uint_desc.Format = wide ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;
    ID3D12Device* const dev = device.Get();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(layers);
    u64 layer_data_bytes = 0;
    dev->GetCopyableFootprints(through_uint ? &uint_desc : &desc, 0, layers, 0,
                               footprints.data(), nullptr, nullptr, &layer_data_bytes);

    BcVolumeProbe probe{};
    probe.width = shape.width;
    probe.height = shape.height;
    probe.slices = layers;
    probe.variant = variant;
    probe.salt = Hash(++bc_volume_probe_count * 0x85EBCA6BU);
    probe.array = true;
    probe.block_bytes = block_bytes;

    // Every resource before any command, so a failure releases them unused.
    const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    if (variant == ArrayCommittedCopies || variant == ArrayFromUint) {
        if (FAILED(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&probe.owned_texture)))) {
            skip("the texture could not be created");
            return;
        }
    } else if (variant == ArrayAliasedUint) {
        const D3D12_RESOURCE_ALLOCATION_INFO bc_info = dev->GetResourceAllocationInfo(0, 1, &desc);
        const D3D12_RESOURCE_ALLOCATION_INFO uint_info =
            dev->GetResourceAllocationInfo(0, 1, &uint_desc);
        LOG_INFO(Render, "D3D12: BC array probe {}x{}x{}: compressed {} bytes (alignment {}), "
                 "integer {} bytes (alignment {})", shape.width, shape.height, layers,
                 bc_info.SizeInBytes, bc_info.Alignment, uint_info.SizeInBytes,
                 uint_info.Alignment);
        const D3D12_HEAP_DESC heap_desc{
            .SizeInBytes = (std::max)(bc_info.SizeInBytes, uint_info.SizeInBytes),
            .Properties = default_heap,
            .Alignment = (std::max)(bc_info.Alignment, uint_info.Alignment),
            .Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES,
        };
        if (FAILED(dev->CreateHeap(&heap_desc, IID_PPV_ARGS(&probe.heap))) ||
            FAILED(dev->CreatePlacedResource(probe.heap.Get(), 0, &desc,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&probe.owned_texture))) ||
            FAILED(dev->CreatePlacedResource(probe.heap.Get(), 0, &uint_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&probe.uint_texture)))) {
            skip("the aliased textures could not be created");
            return;
        }
    } else {
        probe.texture = texture_allocator.Create(desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 {.bytes = block_bytes, .width = 4, .height = 4},
                                                 cpu_heap);
        if (!probe.texture.resource) {
            skip("the texture could not be created");
            return;
        }
        if (cpu_heap && !probe.texture.cpu_writable) {
            skip("textures do not get a CPU-writable heap here");
            texture_allocator.DeferRelease(std::move(probe.texture));
            return;
        }
    }
    const auto release_scratch = [&] {
        if (probe.texture.resource) {
            texture_allocator.DeferRelease(std::move(probe.texture));
        }
    };
    if (variant == ArrayFromUint &&
        FAILED(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uint_desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&probe.uint_texture)))) {
        skip("the integer texture could not be created");
        return;
    }
    ID3D12Resource* const texture =
        probe.owned_texture ? probe.owned_texture.Get() : probe.texture.resource.Get();

    if (variant != ArrayCpuWrite) {
        const bool block_data = variant == ArrayBlockCopies;
        const u64 data_bytes = block_data ? MARKER_STRIDE * total : layer_data_bytes;
        HRESULT data_hr = S_OK;
        probe.markers = CreateCommittedBuffer(dev, data_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              D3D12_RESOURCE_FLAG_NONE, "BC array probe data",
                                              &data_hr);
        u8* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        if (!probe.markers ||
            FAILED(probe.markers->Map(0, &no_read, reinterpret_cast<void**>(&mapped)))) {
            skip("no memory for the data");
            release_scratch();
            return;
        }
        std::memset(mapped, 0, static_cast<size_t>(data_bytes));
        for (u32 index = 0; index < total; ++index) {
            const Marker marker = MakeMarkerFor(block_bytes, probe.salt, index);
            const auto [x, y] = PlaceOf(index % SLOTS, shape.width, shape.height);
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layer_footprint = footprints[index / SLOTS];
            const u64 offset = block_data ? MARKER_STRIDE * index
                                          : layer_footprint.Offset +
                                                u64{y / 4} * layer_footprint.Footprint.RowPitch +
                                                u64{x / 4} * block_bytes;
            std::memcpy(mapped + offset, marker.block.data(), block_bytes);
        }
        probe.markers->Unmap(0, nullptr);
    }
    const u64 result_bytes = Common::AlignUp(total * 4ULL, 256ULL);
    probe.results = CreateTransferBuffer(dev, result_bytes, true);
    probe.readback = CreateCommittedBuffer(dev, result_bytes, D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_FLAG_NONE, "BC array probe readback");
    if (!probe.results || !probe.readback) {
        skip("no memory for the results");
        release_scratch();
        return;
    }
    if (variant == ArrayCpuWrite) {
        HRESULT write_hr = S_OK;
        for (u32 layer = 0; layer < layers && SUCCEEDED(write_hr); ++layer) {
            // Map with no pointer keeps a texture of unknown layout mapped for the write.
            write_hr = texture->Map(layer, nullptr, nullptr);
            for (u32 slot = 0; slot < SLOTS && SUCCEEDED(write_hr); ++slot) {
                const Marker marker = MakeMarkerFor(block_bytes, probe.salt, layer * SLOTS + slot);
                const auto [x, y] = PlaceOf(slot, shape.width, shape.height);
                const D3D12_BOX box{x, y, 0, x + 4, y + 4, 1};
                write_hr = texture->WriteToSubresource(layer, &box, marker.block.data(),
                                                       block_bytes, block_bytes);
            }
            texture->Unmap(layer, nullptr);
        }
        if (FAILED(write_hr)) {
            LOG_INFO(Render, "D3D12: BC array probe {}x{}x{} [{}] not run: the write was refused "
                     "({:#010x})", shape.width, shape.height, layers,
                     ARRAY_VARIANT_NAMES[variant], static_cast<u32>(write_hr));
            release_scratch();
            return;
        }
    }

    auto* const commands = scheduler.CommandList();
    const auto copy_layers = [&](ID3D12Resource* destination) {
        D3D12_TEXTURE_COPY_LOCATION src{.pResource = probe.markers.Get(),
                                        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        D3D12_TEXTURE_COPY_LOCATION dst{.pResource = destination,
                                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        for (u32 layer = 0; layer < layers; ++layer) {
            src.PlacedFootprint = footprints[layer];
            dst.SubresourceIndex = layer;
            commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
    };
    if (variant == ArrayCpuWrite) {
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else if (variant == ArrayAliasedUint) {
        D3D12_RESOURCE_BARRIER alias{.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
                                     .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
                                     .Aliasing = {.pResourceBefore = nullptr,
                                                  .pResourceAfter = probe.uint_texture.Get()}};
        commands->ResourceBarrier(1, &alias);
        copy_layers(probe.uint_texture.Get());
        TransitionResource(commands, probe.uint_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_COMMON);
        alias.Aliasing = {.pResourceBefore = probe.uint_texture.Get(), .pResourceAfter = texture};
        commands->ResourceBarrier(1, &alias);
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else if (variant == ArrayFromUint) {
        copy_layers(probe.uint_texture.Get());
        TransitionResource(commands, probe.uint_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{.pResource = probe.uint_texture.Get(),
                                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{.pResource = texture,
                                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        for (u32 layer = 0; layer < layers; ++layer) {
            src.SubresourceIndex = layer;
            dst.SubresourceIndex = layer;
            commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        if (variant == ArrayBlockCopies) {
            D3D12_TEXTURE_COPY_LOCATION src{.pResource = probe.markers.Get(),
                                            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            D3D12_TEXTURE_COPY_LOCATION dst{.pResource = texture,
                                            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            for (u32 index = 0; index < total; ++index) {
                const auto [x, y] = PlaceOf(index % SLOTS, shape.width, shape.height);
                src.PlacedFootprint = {.Offset = MARKER_STRIDE * index,
                                       .Footprint = {.Format = footprints[0].Footprint.Format,
                                                     .Width = 4, .Height = 4, .Depth = 1,
                                                     .RowPitch =
                                                         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
                dst.SubresourceIndex = index / SLOTS;
                commands->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
            }
        } else {
            copy_layers(texture);
        }
        TransitionResource(commands, texture, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    TransitionBuffer(commands, probe.results.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv = view_descriptors.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = wide ? DXGI_FORMAT_BC6H_UF16 : DXGI_FORMAT_BC1_UNORM;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray = {.MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0,
                               .ArraySize = layers, .PlaneSlice = 0,
                               .ResourceMinLODClamp = 0.0f};
    dev->CreateShaderResourceView(texture, &srv_desc, srv);
    blit_helper->ProbeArrayLayers({.source = srv,
                                   .destination = probe.results->GetGPUVirtualAddress(),
                                   .width = shape.width,
                                   .height = shape.height,
                                   .layers = layers,
                                   .levels = 1,
                                   .volume = false});
    view_descriptors.Free(srv);
    TransitionBuffer(commands, probe.results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(probe.readback.Get(), 0, probe.results.Get(), 0, total * 4ULL);
    probe.tick = scheduler.CurrentTick();
    bc_volume_probe = std::move(probe);
}

} // namespace D3D12
