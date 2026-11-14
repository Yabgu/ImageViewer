/*
 * iwi_file_test.cpp — compile-time layout and sizing tests for IwiFile.h.
 */

#include "IwiFile.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

/*
 * Compile-time static assertions ensuring struct layout, sizes and packing match.
 */
static_assert(sizeof(IwiFileHeader) == 42);
static_assert(sizeof(IwiFileComponent) == 6);
static_assert(sizeof(IwiFileHeader) == IWI_FILE_HEADER_BYTES);
static_assert(sizeof(IwiFileComponent) == IWI_FILE_COMPONENT_BYTES);
static_assert(sizeof(IWStorageLayout) == 8);
static_assert(sizeof(IwiMetaHeader) == 8);
static_assert(sizeof(IWCameraMeta) == 48);

static_assert(offsetof(IwiFileHeader, magic) == 0);
static_assert(offsetof(IwiFileHeader, width) == 4);
static_assert(offsetof(IwiFileHeader, height) == 8);
static_assert(offsetof(IwiFileHeader, metaBytes) == 12);
static_assert(offsetof(IwiFileHeader, storageLayout) == 16);
static_assert(offsetof(IwiFileHeader, payloadEncoding) == 24);
static_assert(offsetof(IwiFileHeader, colorSpace) == 32);
static_assert(offsetof(IwiFileHeader, reserved) == 33);
static_assert(offsetof(IwiFileHeader, componentCount) == 34);
static_assert(offsetof(IwiFileHeader, components) == 36);

static_assert(offsetof(IwiFileComponent, semantic) == 0);
static_assert(offsetof(IwiFileComponent, componentClass) == 1);
static_assert(offsetof(IwiFileComponent, bitOffset) == 2);
static_assert(offsetof(IwiFileComponent, bitWidth) == 4);

int main()
{
    std::printf("iwi_file_test: struct layout and size assertions passed\n");
    return 0;
}
