module;

export module media.ViewCorrespondence;
import meida.Buffer;
import media.Correspondence;

namespace media {
    // Represents a specific event/message for viewing (e.g., viewing a buffer)
    export class ViewCorrespondence : public Correspondence {
    public:
        Buffer* buffer;
        ViewCorrespondence(Buffer* buf) : buffer(buf) {}
        int type() const override { return 1; } // Example type ID for view events
        virtual ~ViewCorrespondence() = default;
    };
}
