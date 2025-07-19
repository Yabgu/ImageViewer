#pragma once
#include "View.ixx"
#include "Processor.ixx"

export module media.Scene;

namespace media {
    // Scene ties together a view and a processor (could be extended for more complex pipelines)
    export template <class TView, class TProcessor>
    class Scene {
        TView view;
        TProcessor processor;
    public:
        Scene(const TView& v, const TProcessor& p) : view(v), processor(p) {}
        void run(Buffer& buffer) {
            processor.process(buffer);
            view.display(buffer);
        }
    };
}
