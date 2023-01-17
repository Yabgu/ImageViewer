
export module media;

namespace media
{
    export class Correspondence // Buffer, Transaction, Operation, shader
    {
    public:
        virtual ~Correspondence() = default;
    };

    export class View // File, screen, image, ....
    {
    public:
        virtual ~View() = default;
    };

    // StringFormatter, TransactionBuilder, ImageGenerator
    export class Corresponder
    {
    public:
        virtual ~Corresponder() = default;

        virtual void interpret(Correspondence&) = 0;
    };


    export template <class View, class Corresponder>
    class Scene
    {
        Corresponder corresponder;
    public:

    };
}
