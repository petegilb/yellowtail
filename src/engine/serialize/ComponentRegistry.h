//
// Maps each component's serialId to a function that builds one, so the loader can
// recreate components from a scene file.
//

#ifndef YELLOWTAIL_COMPONENTREGISTRY_H
#define YELLOWTAIL_COMPONENTREGISTRY_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "../Component.h"

namespace ytail {
    class ComponentRegistry {
    public:
        // Link a serial id to type T. Pass T::SerialId as the id.
        template<typename T>
        void registerType(const std::string& id) {
            factories[id] = []() -> std::unique_ptr<Component> { return std::make_unique<T>(); };
        }

        // Build a new component for the given serial id, or nullptr if the id is unknown.
        [[nodiscard]] std::unique_ptr<Component> create(const std::string& id) const;

        // Register every built-in component. Called once at startup so there's one clear list.
        void registerBuiltins();

    private:
        std::unordered_map<std::string, std::function<std::unique_ptr<Component>()>> factories;
    };
} // ytail

#endif //YELLOWTAIL_COMPONENTREGISTRY_H
