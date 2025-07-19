#pragma once
#include "Correspondence.ixx"

export module media.CorrespondenceInterpreter;

namespace media {
    // Represents something that can interpret or handle a correspondence/event
    export class CorrespondenceInterpreter {
    public:
        virtual ~CorrespondenceInterpreter() = default;
        virtual void interpret(Correspondence&) = 0;
    };
}
