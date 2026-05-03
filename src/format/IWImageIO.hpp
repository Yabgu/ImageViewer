#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

#include "ImagePluginDef.h"
#include "iw_image.pb.h"

// Conversion helpers between the plain-C image ABI (ImagePluginDef.h) and the
// protobuf-serialised .iwi file format (iw_image.proto).

namespace iw {

// ── C ABI <-> proto enum conversions ─────────────────────────────────────────

inline ComponentSemantic ToProtoSemantic(IWComponentSemantic s)
{
    // Numeric values are identical between the C ABI and the proto enum.
    return static_cast<ComponentSemantic>(s);
}

inline IWComponentSemantic FromProtoSemantic(ComponentSemantic s)
{
    return static_cast<IWComponentSemantic>(s);
}

inline ComponentClass ToProtoClass(IWComponentClass c)
{
    return static_cast<ComponentClass>(c);
}

inline IWComponentClass FromProtoClass(ComponentClass c)
{
    return static_cast<IWComponentClass>(c);
}

inline ColorSpace ToProtoColorSpace(ImageColorSpace cs)
{
    switch (cs) {
    case IMAGE_COLOR_SPACE_SRGB:    return COLOR_SPACE_SRGB;
    case IMAGE_COLOR_SPACE_LINEAR:  return COLOR_SPACE_LINEAR;
    case IMAGE_COLOR_SPACE_HDR_PQ:  return COLOR_SPACE_HDR_PQ;
    case IMAGE_COLOR_SPACE_HDR_HLG: return COLOR_SPACE_HDR_HLG;
    default:                        return COLOR_SPACE_UNKNOWN;
    }
}

inline ImageColorSpace FromProtoColorSpace(ColorSpace cs)
{
    switch (cs) {
    case COLOR_SPACE_SRGB:    return IMAGE_COLOR_SPACE_SRGB;
    case COLOR_SPACE_LINEAR:  return IMAGE_COLOR_SPACE_LINEAR;
    case COLOR_SPACE_HDR_PQ:  return IMAGE_COLOR_SPACE_HDR_PQ;
    case COLOR_SPACE_HDR_HLG: return IMAGE_COLOR_SPACE_HDR_HLG;
    default:                  return IMAGE_COLOR_SPACE_UNKNOWN;
    }
}

// ── IWImageFormat <-> proto ImageFormat ──────────────────────────────────────

inline ImageFormat ToProtoFormat(const IWImageFormat& fmt)
{
    ImageFormat pf;
    pf.set_component_count(fmt.componentCount);
    pf.set_bits_per_pixel(fmt.bitsPerPixel);
    pf.set_storage_layout(
        fmt.storageLayout == IW_STORAGE_PLANAR ? STORAGE_PLANAR : STORAGE_INTERLEAVED);
    for (uint16_t i = 0; i < fmt.componentCount && i < IW_MAX_COMPONENTS; ++i) {
        ComponentDef* cd = pf.add_components();
        cd->set_semantic(ToProtoSemantic(fmt.components[i].semantic));
        cd->set_component_class(ToProtoClass(fmt.components[i].componentClass));
        cd->set_bit_offset(fmt.components[i].bitOffset);
        cd->set_bit_width(fmt.components[i].bitWidth);
    }
    return pf;
}

inline IWImageFormat FromProtoFormat(const ImageFormat& pf)
{
    IWImageFormat fmt = {};
    fmt.componentCount = static_cast<uint16_t>(pf.component_count());
    fmt.bitsPerPixel   = static_cast<uint16_t>(pf.bits_per_pixel());
    fmt.storageLayout  = (pf.storage_layout() == STORAGE_PLANAR)
                         ? IW_STORAGE_PLANAR : IW_STORAGE_INTERLEAVED;

    const int n = std::min(pf.components_size(), static_cast<int>(IW_MAX_COMPONENTS));
    for (int i = 0; i < n; ++i) {
        fmt.components[i].semantic       = FromProtoSemantic(pf.components(i).semantic());
        fmt.components[i].componentClass = FromProtoClass(pf.components(i).component_class());
        fmt.components[i].bitOffset      = static_cast<uint16_t>(pf.components(i).bit_offset());
        fmt.components[i].bitWidth       = static_cast<uint16_t>(pf.components(i).bit_width());
    }
    return fmt;
}

// ── Build proto Image from raw pixel data ────────────────────────────────────

inline Image BuildProtoImage(int width, int height,
                              ImageColorSpace colorSpace,
                              const IWImageFormat& fmt,
                              const uint8_t* pixels, size_t pixelBytes)
{
    Image img;
    img.set_width(static_cast<uint32_t>(width));
    img.set_height(static_cast<uint32_t>(height));
    img.set_color_space(ToProtoColorSpace(colorSpace));
    *img.mutable_format() = ToProtoFormat(fmt);
    img.set_pixel_data(pixels, pixelBytes);
    return img;
}

// ── .iwi file I/O ─────────────────────────────────────────────────────────────

inline void SaveIWI(const std::string& path, const Image& img)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        throw std::runtime_error("Cannot open for writing: " + path);
    if (!img.SerializeToOstream(&ofs))
        throw std::runtime_error("Failed to serialize image to: " + path);
}

inline Image LoadIWI(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        throw std::runtime_error("Cannot open for reading: " + path);
    Image img;
    if (!img.ParseFromIstream(&ifs))
        throw std::runtime_error("Failed to parse image from: " + path);
    return img;
}

} // namespace iw
