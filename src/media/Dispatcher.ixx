#pragma once
#include <map>
#include <vector>
#include "Correspondence.ixx"
#include "CorrespondenceInterpreter.ixx"

export module media.Dispatcher;

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
