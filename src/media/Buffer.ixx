#pragma once
#include <vector>
#include <cstddef>
#include "Correspondence.ixx"

export module media.Buffer;

namespace media {
    // Represents a buffer holding N-dimensional data (image, audio, prompt, etc.)
    export class Buffer : public Correspondence {
    public:
        virtual ~Buffer() = default;
        virtual std::vector<int> shape() const = 0;
        virtual int dim(size_t axis) const { return shape().at(axis); }
        // Add more methods as needed (e.g., element access)
    };
}
