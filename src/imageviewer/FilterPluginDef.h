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

/* ── Dithering algorithm selection ───────────────────────────────────────── */
typedef uint32_t IWDitherMode;
enum {
    IW_DITHER_NONE            = 0, /* truncate / round to target bit depth          */
    IW_DITHER_ORDERED         = 1, /* Bayer-matrix ordered dithering                */
    IW_DITHER_FLOYD_STEINBERG = 2, /* error-diffusion (Floyd-Steinberg)             */
    IW_DITHER_PWM             = 3  /* temporal/PWM dither – produces multiple frames */
};

/* ── Filter options ───────────────────────────────────────────────────────── */
typedef struct IWFilterOptions {
    uint32_t     version;    /* ABI version; set to IW_FILTER_OPTIONS_VERSION      */
    IWDitherMode ditherMode; /* dithering algorithm to apply                       */
    uint32_t     pwmBits;    /* extra virtual bits for PWM: 1→2 frames, 2→4, …    */
} IWFilterOptions;

#define IW_FILTER_OPTIONS_VERSION 1u

/* ── Output frame set ─────────────────────────────────────────────────────── */
/*
 * A set of output frames produced by FilterImage.
 *
 *   frameCount == 1  for non-PWM modes.
 *   frameCount == 2^pwmBits  for IW_DITHER_PWM.
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
