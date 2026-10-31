#pragma once

/* Small ownership helper for asynchronous filter jobs.
 *
 * The worker captures a shared_ptr to the source image by value.  Replacing
 * Window::source_image_ while the worker runs is therefore safe: the old Image
 * stays alive until the worker is done.  The result is still generation-tagged
 * by the caller; IsCurrentFilterResult() is the stale-result policy.
 */

#include <cstdint>
#include <future>
#include <memory>
#include <utility>

namespace FilterJob {

template <typename ImageT, typename ComputeFn>
auto Start(std::shared_ptr<const ImageT> image, ComputeFn&& compute)
{
    return std::async(std::launch::async,
        [image = std::move(image), compute = std::forward<ComputeFn>(compute)]() mutable {
            return compute(*image);
        });
}

inline bool IsCurrentFilterResult(std::uint64_t jobGeneration,
                                  std::uint64_t currentGeneration) noexcept
{
    return jobGeneration == currentGeneration;
}

} // namespace FilterJob
