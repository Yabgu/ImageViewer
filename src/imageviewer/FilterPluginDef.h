#pragma once
#include <cstdint>
#include <cstddef>
#include "ImagePluginDef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Export macro ─────────────────────────────────────────────────────────── */
#ifndef FILTERPLUGIN_API
#ifdef _WIN32
#  define FILTERPLUGIN_API __declspec(dllexport)
#else
#  define FILTERPLUGIN_API __attribute__((visibility("default")))
#endif
#endif

/* ── Screen / display capabilities ───────────────────────────────────────── */
typedef struct IWScreenInfo {
    uint8_t bitsPerChannel; /* bits per colour channel reported by the GL context */
    uint8_t channelCount;   /* number of colour channels (typically 3 or 4)       */
} IWScreenInfo;

/* ── Typed filter argument ────────────────────────────────────────────────── */
/*
 * IWFilterArg is a self-describing, named, typed value passed to a filter
 * plugin (analogous to IWComponentDef for pixel components).
 *
 * Basic scalar types are stored by value in the union.
 * Struct types (IWImageFormat) are always stored as a const pointer.
 *
 * Argument names are NUL-terminated strings, capped at
 * IW_FILTER_ARG_NAME_MAX - 1 usable characters.
 */

/* Maximum length of an argument name including the NUL terminator */
#define IW_FILTER_ARG_NAME_MAX 64

/* Type discriminator for IWFilterArg.value */
typedef uint32_t IWFilterArgType;
enum {
    IW_FILTER_ARG_INT32        = 0,  /* int32_t scalar                */
    IW_FILTER_ARG_INT64        = 1,  /* int64_t scalar                */
    IW_FILTER_ARG_UINT32       = 2,  /* uint32_t scalar               */
    IW_FILTER_ARG_UINT64       = 3,  /* uint64_t scalar               */
    IW_FILTER_ARG_FLOAT32      = 4,  /* float (32-bit IEEE)           */
    IW_FILTER_ARG_FLOAT64      = 5,  /* double (64-bit IEEE)          */
    IW_FILTER_ARG_IMAGE_FORMAT = 6   /* const IWImageFormat* pointer  */
};

typedef struct IWFilterArg {
    char            name[IW_FILTER_ARG_NAME_MAX]; /* argument name (NUL-terminated) */
    IWFilterArgType type;                          /* discriminates the union below  */
    union {
        int32_t              i32;
        int64_t              i64;
        uint32_t             u32;
        uint64_t             u64;
        float                f32;
        double               f64;
        const IWImageFormat* imageFormat; /* IW_FILTER_ARG_IMAGE_FORMAT      */
    } value;
} IWFilterArg;

/* ── Filter invocation options ────────────────────────────────────────────── */
/*
 * IWFilterOptions passes a generic, extensible argument list to a filter
 * plugin.  Plugins discover their own parameters by name from argv[].
 * The host does not need to know which algorithm a plugin implements.
 *
 *   version  — must be set to IW_FILTER_OPTIONS_VERSION.
 *   argc     — number of entries in argv[].
 *   argv     — array of typed arguments; may be NULL when argc == 0.
 */
typedef struct IWFilterOptions {
    uint32_t          version; /* ABI version; set to IW_FILTER_OPTIONS_VERSION  */
    uint32_t          argc;    /* number of arguments in argv                    */
    const IWFilterArg* argv;  /* typed argument list; NULL when argc == 0        */
} IWFilterOptions;

#define IW_FILTER_OPTIONS_VERSION 1u

/* ── Output frame set ─────────────────────────────────────────────────────── */
/*
 * A set of output frames produced by FilterImage.
 *
 * frameCount is 1 for single-output plugins, or >1 when a plugin produces
 * multiple temporally-distinct frames (e.g., for temporal dithering).
 *
 * frames[] is a heap array allocated by the plugin (malloc).
 * Each frames[i] is a separately malloc-allocated ImagePluginData whose
 * .data buffer is also malloc-allocated.
 * The host must call FreeFilterImageSet when it is done with the set.
 */
typedef struct IWFilterImageSet {
    uint32_t           frameCount; /* number of frames in this set              */
    ImagePluginData**  frames;     /* array of frameCount pointers              */
} IWFilterImageSet;

/* ── Plugin function-pointer typedefs ────────────────────────────────────── */
typedef IWFilterImageSet* (*FilterImageFunc)(
    const ImagePluginData*  src,
    const IWScreenInfo*     screen,
    const IWFilterOptions*  options);

typedef void        (*FreeFilterImageSetFunc)(IWFilterImageSet*);
typedef const char* (*FilterPluginGetLastErrorFunc)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
