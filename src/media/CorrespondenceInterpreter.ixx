module;

export module media.CorrespondenceInterpreter;
import media.Correspondence;


namespace media {
    // Represents something that can interpret or handle a correspondence/event
    export class CorrespondenceInterpreter {
    public:
        virtual ~CorrespondenceInterpreter() = default;
        virtual void interpret(Correspondence&) = 0;
    };
}
