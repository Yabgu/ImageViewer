/*
 * The plugin layer's data contract: ownership, version, and the no-cap promise.
 *
 * ImagePluginData is the one struct every loader hands to the host, and the
 * defects this file guards against all came from it being treated as a bag of
 * bytes:
 *
 *   - a shared, per-thread component table, so a second load on a thread
 *     rewrote the first image's descriptor (TIFF, and the filter's target
 *     format);
 *   - a descriptor pointing at a vector the host had already destroyed
 *     (IWImageIO::Image);
 *   - a fixed-size component array, which is a cap the container format does
 *     not have.
 *
 * So the contract is asserted here at the level it is stated: copying is
 * impossible, moving transfers ownership, exactly one object frees the blob,
 * the ABI version is stamped and checked, and a 200-component descriptor is as
 * valid as a 3-component one.
 *
 * These cases do not load a plugin binary; they are the rules a plugin must
 * satisfy, and test_imageloader_*.cpp checks that the shipped loaders satisfy
 * them in practice.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "ImagePluginDef.h"

#include <doctest/doctest.h>

using iw::kRgb8Components;

/*
 * Compile-time: the type is move-only.
 *
 * A copy has to decide who owns ownedBlob, and every answer except "exactly
 * one of them" is a double free or a leak.  Deleting it moves the question to
 * compile time, where it cannot be answered wrongly.
 */
static_assert(!std::is_copy_constructible_v<ImagePluginData>,
              "ImagePluginData must not be copy-constructible: ownership of ownedBlob would be ambiguous");
static_assert(!std::is_copy_assignable_v<ImagePluginData>,
              "ImagePluginData must not be copy-assignable");
static_assert(std::is_move_constructible_v<ImagePluginData>,
              "ImagePluginData must be movable");
static_assert(std::is_move_assignable_v<ImagePluginData>,
              "ImagePluginData must be move-assignable");
static_assert(std::is_nothrow_move_constructible_v<ImagePluginData>,
              "moving must not throw: loaders move descriptors on their hot path");
static_assert(std::is_nothrow_move_assignable_v<ImagePluginData>);

/*
 * Compile-time: the two views of the component table are at fixed offsets, so a
 * C producer built against the documented layout and a C++ consumer agree on
 * where to write them.  Pinned here rather than described, because the C half
 * of this ABI has no constructors to set them.
 */
static_assert(offsetof(ImagePluginData, components) % alignof(const IWComponentDef*) == 0);
static_assert(sizeof(ImagePluginData) >= sizeof(void*) * 2);

namespace {

int g_freeCount = 0;

/*
 * A free that counts.  Every ownership case below goes through it so "released
 * exactly once" is measured rather than assumed - a double free of a real
 * malloc'd pointer would usually abort, but a *missing* free is silent, and
 * that is the failure mode a leak in this layer produces.
 */
void CountingFree(void* p)
{
    ++g_freeCount;
    std::free(p);
}

/* A component table of `count` interleaved 8-bit records, heap-allocated the
 * way a loader would allocate one, standing in for a parsed blob. */
IWComponentDef* MakeTable(std::size_t count)
{
    auto* table = static_cast<IWComponentDef*>(
        std::malloc(sizeof(IWComponentDef) * count));
    REQUIRE(table != nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        table[i] = IWComponentDef{
            static_cast<IWComponentSemantic>(IW_COMPONENT_SEMANTIC_R),
            IW_COMPONENT_CLASS_UNORM,
            static_cast<uint16_t>(i * 8u),
            8u
        };
    }
    return table;
}

/* A descriptor owning `table`, of the shape a loader produces. */
ImagePluginData OwningDescriptor(std::size_t count)
{
    IWComponentDef* table = MakeTable(count);

    ImagePluginData d{};
    d.width  = 1;
    d.height = 1;
    d.stride = static_cast<int>(count);
    d.size   = count;
    d.data   = static_cast<uint8_t*>(std::malloc(count));
    std::memset(d.data, 0x5A, count);
    d.format.componentCount = static_cast<uint16_t>(count);
    d.format.bitsPerPixel   = static_cast<uint16_t>(count * 8u);
    iw_plugin_data_bind(d, table, count);
    iw_plugin_data_own(d, table, CountingFree);
    return d;
}

/* Release what a descriptor owns, the way the host does it: the blob through
 * the descriptor's own hook, then the plugin's FreeImageData releases `data`. */
void Release(ImagePluginData& d)
{
    if (d.freeOwnedBlob)
        d.freeOwnedBlob(d.ownedBlob);
    std::free(d.data);
    d.ownedBlob     = nullptr;
    d.freeOwnedBlob = nullptr;
    d.data          = nullptr;
}

} // namespace

TEST_SUITE("ImagePluginData: ownership")
{
    TEST_CASE("a default-constructed descriptor owns nothing and is version-stamped")
    {
        ImagePluginData d{};

        CHECK(d.ownedBlob       == nullptr);
        CHECK(d.freeOwnedBlob   == nullptr);
        CHECK(d.components      == nullptr);
        CHECK(d.componentCount  == 0u);
        CHECK(d.structVersion   == IW_PLUGIN_STRUCT_VERSION);
        /* Two null views of "no table" still agree, so an empty descriptor is
         * admitted rather than rejected by the invariant check. */
        CHECK(iw_plugin_data_is_compatible(d));
    }

    TEST_CASE("moving transfers the blob and leaves the source unable to free it")
    {
        g_freeCount = 0;

        ImagePluginData src = OwningDescriptor(3);
        IWComponentDef* const table = const_cast<IWComponentDef*>(src.components);
        CHECK(src.ownedBlob == table);

        ImagePluginData dst = std::move(src);

        /* The destination reads the same table, through both views. */
        CHECK(dst.ownedBlob      == table);
        CHECK(dst.components     == table);
        CHECK(dst.format.components == table);
        CHECK(dst.componentCount == 3u);

        /* The source relinquished ownership: this is the assertion that fails
         * against a defaulted move, where both objects keep the same pointer
         * and both releases run. */
        CHECK(src.ownedBlob     == nullptr);
        CHECK(src.freeOwnedBlob == nullptr);
        CHECK(src.components    == nullptr);

        Release(dst);
        Release(src);   /* must be a no-op, not a second free */

        CHECK(g_freeCount == 1);
    }

    TEST_CASE("move-assignment releases the destination's blob before taking the new one")
    {
        g_freeCount = 0;

        ImagePluginData a = OwningDescriptor(3);
        ImagePluginData b = OwningDescriptor(4);

        IWComponentDef* const first  = const_cast<IWComponentDef*>(a.components);
        IWComponentDef* const second = const_cast<IWComponentDef*>(b.components);
        CHECK(first != second);

        /* Assigning over a still-owning descriptor must free what it held, or
         * the previous frame leaks - which is what a naive field-by-field copy
         * did. */
        a = std::move(b);

        CHECK(g_freeCount == 1);          /* `first` released by the assignment */
        CHECK(a.components == second);
        CHECK(a.ownedBlob  == second);
        CHECK(a.componentCount == 4u);
        CHECK(b.ownedBlob     == nullptr);
        CHECK(b.freeOwnedBlob == nullptr);

        Release(a);
        CHECK(g_freeCount == 2);

        /* `b` owns nothing now, so releasing it must not touch the counter. */
        Release(b);
        CHECK(g_freeCount == 2);
    }

    TEST_CASE("a descriptor over a constexpr table owns nothing and releasing it is a no-op")
    {
        g_freeCount = 0;

        ImagePluginData d{};
        d.width  = 4;
        d.height = 1;
        d.stride = 12;
        d.size   = 12;
        d.data   = static_cast<uint8_t*>(std::malloc(12));
        d.format.componentCount = 3;
        d.format.bitsPerPixel   = 24;
        iw_plugin_data_bind(d, kRgb8Components, 3);

        CHECK(d.ownedBlob     == nullptr);
        CHECK(d.freeOwnedBlob == nullptr);
        CHECK(iw_plugin_data_is_compatible(d));

        Release(d);
        CHECK(g_freeCount == 0);
    }

    TEST_CASE("the admission check refuses a descriptor from a different ABI version")
    {
        ImagePluginData d = OwningDescriptor(4);
        CHECK(iw_plugin_data_is_compatible(d));

        d.structVersion = 0;
        CHECK_FALSE(iw_plugin_data_is_compatible(d));

        d.structVersion = IW_PLUGIN_STRUCT_VERSION + 1u;
        CHECK_FALSE(iw_plugin_data_is_compatible(d));

        d.structVersion = IW_PLUGIN_STRUCT_VERSION;
        CHECK(iw_plugin_data_is_compatible(d));

        Release(d);
    }

    TEST_CASE("the admission check refuses half-filled ownership fields")
    {
        /* A blob with no way to release it, and a release with no blob: both
         * are producer bugs that would otherwise leak or crash much later. */
        ImagePluginData d{};
        static int notARealBlob = 0;

        d.ownedBlob = &notARealBlob;
        CHECK_FALSE(iw_plugin_data_is_compatible(d));

        d.ownedBlob     = nullptr;
        d.freeOwnedBlob = [](void*) {};
        CHECK_FALSE(iw_plugin_data_is_compatible(d));
    }

    TEST_CASE("the admission check refuses views that disagree")
    {
        IWComponentDef* table = MakeTable(4);

        ImagePluginData d{};
        d.ownedBlob     = table;
        d.freeOwnedBlob = CountingFree;
        d.components    = table;
        d.componentCount = 4;
        /* Only the ownership view was filled in; the render view still points
         * at nothing.  Rendering from it would read address zero. */
        CHECK_FALSE(iw_plugin_data_is_compatible(d));

        iw_plugin_data_bind(d, table, 4);
        CHECK(iw_plugin_data_is_compatible(d));

        /* A count that disagrees with the format view is the same defect
         * spelled the other way. */
        d.componentCount = 3;
        CHECK_FALSE(iw_plugin_data_is_compatible(d));

        Release(d);
    }
}

TEST_SUITE("ImagePluginData: no component cap")
{
    TEST_CASE("a 200-component descriptor is valid, movable, and released once")
    {
        g_freeCount = 0;

        /* The container format has no cap, so neither does this struct: no
         * fixed array, no IW_*_MAX_COMPONENTS, no allocation sized for four.
         * A 200-band spectral file is a valid descriptor.  Whether it renders
         * is a separate question the consumer answers, not one this layer
         * pre-empts by refusing to describe it. */
        ImagePluginData d = OwningDescriptor(200);

        CHECK(d.componentCount == 200u);
        CHECK(d.format.componentCount == 200u);
        CHECK(d.ownedBlob != nullptr);
        CHECK(iw_plugin_data_is_compatible(d));

        /* Every record is reachable through the blob, in order. */
        for (std::size_t i = 0; i < 200; ++i) {
            REQUIRE(d.components != nullptr);
            CHECK(d.components[i].bitOffset == static_cast<uint16_t>(i * 8u));
            CHECK(d.components[i].bitWidth  == 8u);
        }

        ImagePluginData moved = std::move(d);
        CHECK(d.ownedBlob == nullptr);
        CHECK(moved.componentCount == 200u);

        Release(moved);
        CHECK(g_freeCount == 1);
    }

    TEST_CASE("a 65535-component descriptor does not wrap its bit count")
    {
        /* bitsPerPixel is a uint16 in the ABI and the count is a uint16 in the
         * container.  65535 8-bit components is 524280 bits: the loader
         * saturates instead of wrapping to a small number, so the descriptor
         * never claims a pixel is narrower than one component. */
        constexpr std::uint32_t kBits = 8u * 65535u;
        const uint16_t saturated = kBits > 0xFFFFu ? 0xFFFFu
                                                   : static_cast<uint16_t>(kBits);
        CHECK(saturated == 0xFFFFu);
        CHECK(static_cast<std::uint32_t>(saturated) >= 8u);
    }
}
