#pragma once
#include <map>
#include <string>

export module media.Correspondence;

namespace media {
    // Represents a generic event or message in the system (base for all data types)
    export class Correspondence {
    public:
        virtual ~Correspondence() = default;
        // Event type identifier for dispatching
        virtual int type() const = 0;
        // Optional metadata (e.g., timestamp, user data)
        virtual const std::map<std::string, std::string>& metadata() const { static std::map<std::string, std::string> dummy; return dummy; }
    };
}
