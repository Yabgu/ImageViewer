
export module media;

namespace media
{
    export class Correspondance // Buffer, Transaction, Operation, shader
    {
    public:
        virtual ~Correspondance() = default;
        virtual void correspond() = 0;
    };

    export class Corresponder // String builder, transaction builder, shader compiler
    {
    public:
        virtual ~Corresponder() = default;
    };

    export class View // File, screen, buffer, ....
    {
    public:
        virtual ~View() = default;

        virtual void sync() = 0;
    };

    export template <class View, class Corresponder>
    class Scene
    {
        Corresponder corresponder;
    public:

    };
}
