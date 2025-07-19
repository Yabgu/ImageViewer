#pragma once
#include "Buffer.ixx"

export module media.Processor;

namespace media {
    // Represents a processing operation (filter, transformation, etc.)
    export class Processor {
    public:
        virtual ~Processor() = default;
        virtual void process(Buffer&) = 0;
    };
}
