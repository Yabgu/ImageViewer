#pragma once

/*
 * IwiFile.h — On-disk container format for .iwi images.
 *
 * Defines the on-disk structures and constants for the .iwi format.
 * An .iwi file begins with a packed header (36 bytes base + 6 bytes per component),
 * followed by an optional metadata block of metaBytes, followed by pixel data.
 * All multi-byte values are little-endian.
 */

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#include "ImagePluginDef.h"

/* ── Container Constants ─────────────────────────────────────────────────── */

#define IWI_FILE_MAGIC              "IWI1"
#define IWI_FILE_MAGIC_BYTES        4u
#define IWI_FILE_BASE_HEADER_BYTES  36u
#define IWI_FILE_HEADER_BYTES       42u  /* Base header (36B) + 1 component (6B) */
#define IWI_FILE_COMPONENT_BYTES    6u
#define IWI_FILE_MIN_COMPONENTS     1u

#define IWI_PAYLOAD_RAW             "raw"
#define IWI_PAYLOAD_LZ4             "lz4"
#define IWI_PAYLOAD_ZSTD            "zstd"

/* ── Extensible Metadata Entry & Tags ────────────────────────────────────── */

#define IWI_META_TAG_CAMERA 0x414D4143u /* "CAMA": 3D Camera intrinsics & extrinsics */
#define IWI_META_TAG_CBOR   0x524F4243u /* "CBOR": IETF RFC 8949 nestable binary JSON*/
#define IWI_META_TAG_BSON   0x4E4F5342u /* "BSON": BSON document                    */
#define IWI_META_TAG_MSGP   0x5047534Du /* "MSGP": MessagePack                       */
#define IWI_META_TAG_JSON   0x4E4F534Au /* "JSON": UTF-8 JSON text                   */
#define IWI_META_TAG_TEXT   0x54584554u /* "TEXT": UTF-8 key-value string metadata   */

typedef struct IWCameraMeta {
    float fovDegrees;       /* Vertical FOV in degrees (e.g. 60.0f)     */
    float nearClip;         /* Near clipping plane (e.g. 0.1f)          */
    float farClip;          /* Far clipping plane (e.g. 1000.0f)        */
    float position[3];      /* Camera eye position (X, Y, Z)            */
    float upVector[3];      /* Camera up direction (normalized X, Y, Z) */
    float target[3];        /* Look-at target point (X, Y, Z)           */
} IWCameraMeta;

typedef struct IwiMetaHeader {
    uint32_t tag;    /* FourCC tag identifying entry type */
    uint32_t size;   /* Byte length of entry payload      */
} IwiMetaHeader;

/* ── On-Disk Structs (packed) ────────────────────────────────────────────── */

#pragma pack(push, 1)

typedef struct IwiFileComponent {
    IWComponentSemantic semantic;       /* Channel semantic (IWComponentSemantic, 1 byte) */
    IWComponentClass    componentClass; /* Storage class (IWComponentClass, 1 byte)       */
    uint16_t            bitOffset;      /* Bit offset from start of pixel (2 bytes)       */
    uint16_t            bitWidth;       /* Bit width of component (2 bytes)               */
} IwiFileComponent;

typedef struct IwiFileHeader {
    char             magic[4];                             /*  0: Magic identifier "IWI1" (4 bytes)       */
    uint32_t         width;                                /*  4: Image width in pixels (4 bytes)          */
    uint32_t         height;                               /*  8: Image height in pixels (4 bytes)         */
    uint32_t         metaBytes;                            /* 12: Metadata blob size in bytes (4 bytes)   */
    IWStorageLayout  storageLayout;                        /* 16: Layout & stride (8 bytes, align 8)       */
    char             payloadEncoding[8];                   /* 24: Codec name: "raw\0\0\0\0\0" (8 bytes)    */
    ImageColorSpace  colorSpace;                           /* 32: Color space enum (1 byte, align 1)      */
    uint8_t          reserved;                             /* 33: Reserved padding (1 byte, zeroed)       */
    uint16_t         componentCount;                       /* 34: Channel count (2 bytes, align 2)        */
    IwiFileComponent components[IWI_FILE_MIN_COMPONENTS];  /* 36: Component table (min 1 = 6 bytes)       */
} IwiFileHeader;

#pragma pack(pop)
