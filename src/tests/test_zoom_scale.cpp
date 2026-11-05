#include <doctest/doctest.h>

#include "ZoomScale.hpp"

#include <cmath>

/* The zoom scale is user-visible behaviour: a step must feel the same at 10 %
 * and at 400 %, never doubling the image at one end or disappearing at the
 * other.  These tests pin that policy down. */

TEST_CASE("zoom scale is a quarter octave per step")
{
    const double r = ZoomScale::kStepRatio;

    CHECK(std::pow(r, 4.0) == doctest::Approx(2.0).epsilon(1e-12));
    CHECK(ZoomScale::Step(1.0, 4.0) == doctest::Approx(2.0));
    CHECK(ZoomScale::Step(2.0, -4.0) == doctest::Approx(1.0));
    CHECK(ZoomScale::Step(0.5, 4.0) == doctest::Approx(1.0));

    /* Eight steps make 4x, twelve make 8x: powers of two stay exact stops. */
    CHECK(ZoomScale::Step(1.0, 8.0) == doctest::Approx(4.0));
    CHECK(ZoomScale::Step(1.0, 12.0) == doctest::Approx(8.0));
}

TEST_CASE("one step changes the view by the same ratio at any zoom")
{
    const double low = ZoomScale::Step(0.1, 1.0) / 0.1;
    const double mid = ZoomScale::Step(1.0, 1.0) / 1.0;
    const double high = ZoomScale::Step(4.0, 1.0) / 4.0;

    CHECK(low == doctest::Approx(mid));
    CHECK(high == doctest::Approx(mid));

    /* The old scale added 10 percentage points: +6.7 % at 150 %, and a
     * halving at 20 %.  Neither extreme may come back. */
    CHECK(ZoomScale::Step(1.5, 1.0) / 1.5 > 1.15);
    CHECK(ZoomScale::Step(0.2, -1.0) > 0.15);
}

TEST_CASE("fractional steps zoom smoothly")
{
    const double half = ZoomScale::Step(1.0, 0.5);
    CHECK(half * half == doctest::Approx(ZoomScale::Step(1.0, 1.0)));

    /* A high-resolution wheel may report a fraction of a notch. */
    CHECK(ZoomScale::Step(1.0, 0.1) > 1.0);
    CHECK(ZoomScale::Step(1.0, 0.1) < ZoomScale::Step(1.0, 0.5));
}

TEST_CASE("steps are monotonic and reversible inside the range")
{
    for (double zoom = 0.05; zoom < 10.0; zoom *= 1.7)
    {
        CHECK(ZoomScale::Step(zoom, 1.0) > zoom);
        CHECK(ZoomScale::Step(zoom, -1.0) < zoom);
        CHECK(ZoomScale::Step(ZoomScale::Step(zoom, 3.0), -3.0)
              == doctest::Approx(zoom));
    }
}

TEST_CASE("zoom stays inside its limits")
{
    CHECK(ZoomScale::Clamp(0.001) == ZoomScale::kMinZoom);
    CHECK(ZoomScale::Clamp(1000.0) == ZoomScale::kMaxZoom);

    CHECK(ZoomScale::Step(1.0, 100.0) == ZoomScale::kMaxZoom);
    CHECK(ZoomScale::Step(1.0, -100.0) == ZoomScale::kMinZoom);

    /* At a limit the step stops instead of overshooting or reversing. */
    CHECK(ZoomScale::Step(ZoomScale::kMaxZoom, 1.0) == ZoomScale::kMaxZoom);
    CHECK(ZoomScale::Step(ZoomScale::kMinZoom, -1.0) == ZoomScale::kMinZoom);
}

TEST_CASE("fit-to-window picks the limiting axis")
{
    CHECK(ZoomScale::Fit(4000, 3000, 800, 600) == doctest::Approx(0.2));
    CHECK(ZoomScale::Fit(4000, 3000, 800, 300) == doctest::Approx(0.1));
    CHECK(ZoomScale::Fit(200, 100, 800, 600) == doctest::Approx(4.0));

    /* Small images may not fit above the step ceiling. */
    CHECK(ZoomScale::Fit(16, 16, 800, 600) == ZoomScale::kMaxZoom);

    /* Degenerate sizes fall back to 100 % instead of dividing by zero. */
    CHECK(ZoomScale::Fit(0, 0, 800, 600) == 1.0);
    CHECK(ZoomScale::Fit(800, 600, 0, 0) == 1.0);
}

TEST_CASE("zoom percentage rounds for display")
{
    CHECK(ZoomScale::Percent(1.0) == 100);
    CHECK(ZoomScale::Percent(0.01) == 1);
    CHECK(ZoomScale::Percent(32.0) == 3200);
    CHECK(ZoomScale::Percent(1.0905) == 109);
}
