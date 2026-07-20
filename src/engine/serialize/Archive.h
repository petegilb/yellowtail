//
// One serialize(Archive&) per component handles both saving and loading.
// Components only ever call ar(...), so they never touch the JSON directly.
//

#ifndef YELLOWTAIL_ARCHIVE_H
#define YELLOWTAIL_ARCHIVE_H

#include <nlohmann/json.hpp>

namespace ytail {
    class ResourceManager;

    struct Archive {
        // The JSON object we're saving into or loading from right now.
        nlohmann::json* node = nullptr;
        // true = loading (reading from node), false = saving (writing to node).
        bool isReading = true;
        // File format version (written by SCENE_VERSION in SceneSerializer)
        int version = 1;
        // Used to turn asset paths back into loaded assets. Null when not needed.
        ResourceManager* resources = nullptr;

        [[nodiscard]] bool reading() const { return isReading; }

        // Saves or loads one named field, whichever direction we're going.
        // When loading, a missing field is left at its default.
        template<typename T>
        void operator()(const char* name, T& value) {
            if (isReading) {
                if (node->contains(name)) value = node->at(name).template get<T>();
            } else {
                (*node)[name] = value;
            }
        }
    };
} // ytail

#endif //YELLOWTAIL_ARCHIVE_H
