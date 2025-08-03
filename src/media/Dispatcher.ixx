module;

#include <map>
#include <vector>


export module media.Dispatcher;
import media.Correspondence;
import media.CorrespondenceInterpreter;

namespace media {
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
}
