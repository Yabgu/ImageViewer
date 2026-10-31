#include <doctest/doctest.h>

#include "FilterJob.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace {

struct DummyImage
{
    int value;
};

} // namespace

TEST_CASE("FilterJob keeps the source alive when the owner replaces it")
{
    auto current = std::make_shared<DummyImage>(DummyImage{42});
    std::promise<void> release;
    std::future<void> gate = release.get_future();
    std::atomic<bool> worker_started{false};

    auto job = FilterJob::Start(
        std::shared_ptr<const DummyImage>(std::move(current)),
        [&worker_started, &gate](const DummyImage& image)
        {
            worker_started.store(true);
            gate.wait();
            return image.value;
        });

    while (!worker_started.load())
        std::this_thread::yield();

    /* This is the navigation replacement: the Window slot no longer owns the
     * image that the worker is reading.  The worker only reads it after the
     * gate opens, i.e. after this replacement. */
    current = std::make_shared<DummyImage>(DummyImage{7});
    release.set_value();

    CHECK(job.get() == 42);
}

TEST_CASE("FilterJob generation check rejects stale results")
{
    CHECK(FilterJob::IsCurrentFilterResult(1, 1));
    CHECK_FALSE(FilterJob::IsCurrentFilterResult(1, 2));
    CHECK_FALSE(FilterJob::IsCurrentFilterResult(2, 1));
}
