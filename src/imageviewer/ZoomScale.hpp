#pragma once
/* ── Zoom scale ──────────────────────────────────────────────────────────────
 *
 * Zoom is stored as a linear magnification factor (1.0 = 100 %), but every
 * user-facing step is geometric.  A step multiplies the factor by 2^(1/4)
 * (~1.189), so a wheel notch or a +/- press changes the view by the same
 * *ratio* at 10 % and at 400 %.
 *
 * The old scale added a fixed 10 percentage points per step.  At 10 % that
 * doubled the image, and at 150 % it moved it by 6.7 % — a step the eye cannot
 * see.  A geometric scale removes both extremes.
 *
 * Four steps per doubling keeps round stops reachable: 100 -> 119 -> 141 ->
 * 168 -> 200 %, and four steps back return exactly to 100 %.
 *
 * Step limits are 1 % .. 3200 %.  The old floor of 10 % was arbitrary: a
 * fit-to-window zoom may legitimately sit below it (a 20 000 px image in an
 * 800 px window fits at 4 %), and a floor above the fitted value makes the
 * first zoom-out step jump instead of move.
 */

#include <algorithm>
#include <cmath>

namespace ZoomScale
{
inline constexpr double kMinZoom = 0.01;    /*    1 % */
inline constexpr double kMaxZoom = 32.0;    /* 3200 % */

/* 2^(1/4): four steps per doubling.  Written out because std::pow is not
 * constexpr before C++26; the unit tests check it to 1e-12. */
inline constexpr double kStepRatio = 1.1892071150027210;

inline double Clamp(double zoom) noexcept
{
    return std::clamp(zoom, kMinZoom, kMaxZoom);
}

/* Zoom after `steps` steps; positive zooms in.  Fractional steps are allowed so
 * a high-resolution wheel or touchpad zooms smoothly. */
inline double Step(double zoom, double steps) noexcept
{
    if (!(zoom > 0.0)) return 1.0;            /* no view yet: start from 100 % */
    const double next = zoom * std::pow(kStepRatio, steps);
    if (!std::isfinite(next)) return steps > 0.0 ? kMaxZoom : kMinZoom;
    return Clamp(next);
}

/* Fit-to-window magnification.  Degenerate sizes fall back to 100 % so callers
 * never propagate a division-by-zero or a negative zoom. */
inline double Fit(double imageWidth, double imageHeight,
                  double viewWidth, double viewHeight) noexcept
{
    if (!(imageWidth > 0.0) || !(imageHeight > 0.0) ||
        !(viewWidth > 0.0) || !(viewHeight > 0.0))
        return 1.0;
    return Clamp(std::min(viewWidth / imageWidth, viewHeight / imageHeight));
}

inline int Percent(double zoom) noexcept
{
    return static_cast<int>(std::lround(zoom * 100.0));
}
} /* namespace ZoomScale */
