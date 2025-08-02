module;

#include <vector>
#include <cstddef>
#include <map>
#include <string>

export module media;

namespace media
{
    // Represents a generic event or message in the system (base for all data types)
    export class Correspondence {
    public:
        virtual ~Correspondence() = default;
        // Event type identifier for dispatching
        virtual int type() const = 0;
        // Optional metadata (e.g., timestamp, user data)
        virtual const std::map<std::string, std::string>& metadata() const { static std::map<std::string, std::string> dummy; return dummy; }
    };

    // Represents a buffer holding N-dimensional data (image, audio, prompt, etc.)
    export class Buffer : public Correspondence {
    public:
        virtual ~Buffer() = default;
        virtual std::vector<int> shape() const = 0;
        virtual int dim(size_t axis) const { return shape().at(axis); }
        // Add more methods as needed (e.g., element access)
    };

    // Represents something that can display or output a buffer (window, file, etc.)
    export class View {
    public:
        virtual ~View() = default;
        virtual void display(const Buffer&) = 0;
    };

    // Represents a processing operation (filter, transformation, etc.)
    export class Processor {
    public:
        virtual ~Processor() = default;
        virtual void process(Buffer&) = 0;
    };

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

    // Represents a specific event/message for viewing (e.g., viewing a buffer)
    export class ViewCorrespondence : public Correspondence {
    public:
        Buffer* buffer;
        ViewCorrespondence(Buffer* buf) : buffer(buf) {}
        int type() const override { return 1; } // Example type ID for view events
        virtual ~ViewCorrespondence() = default;
    };

    // Represents something that can interpret or handle a correspondence/event
    export class CorrespondenceInterpreter {
    public:
        virtual ~CorrespondenceInterpreter() = default;
        virtual void interpret(Correspondence&) = 0;
    };

    // Dispatcher for routing events to handlers
    export class Dispatcher {
        std::map<int, std::vector<CorrespondenceInterpreter*>> handlers;
    public:
        void registerHandler(int type, CorrespondenceInterpreter* handler) {
            handlers[type].push_back(handler);
        }
        void dispatch(Correspondence& c) {
            int t = c.type();
            if (handlers.count(t)) {
                for (auto* h : handlers[t]) {
                    h->interpret(c);
                }
            }
        }
    };

    // Example workflow (not compiled, for illustration):
    /*
    // 1. Create a buffer
    class MyBuffer : public Buffer { ... };
    MyBuffer buffer;

    // 2. Wrap it in a view correspondence (event/message)
    ViewCorrespondence viewEvent{&buffer};

    // 3. Pass the event to a dispatcher/interpreter
    class MyInterpreter : public CorrespondenceInterpreter {
    public:
        void interpret(Correspondence& c) override {
            if (auto* v = dynamic_cast<ViewCorrespondence*>(&c)) {
                MyView view;
                MyProcessor processor;
                Scene<MyView, MyProcessor> scene{view, processor};
                scene.run(*v->buffer);
            }
        }
    };
    MyInterpreter interpreter;
    Dispatcher dispatcher;
    dispatcher.registerHandler(1, &interpreter); // 1 = view event type
    dispatcher.dispatch(viewEvent);
    */
}
