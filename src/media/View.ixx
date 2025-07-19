#pragma once
#include "Buffer.ixx"

export module media.View;

namespace media {
    // Represents something that can display or output a buffer (window, file, etc.)
    export class View {
    public:
        virtual ~View() = default;
        virtual void display(const Buffer&) = 0;
    };
}
