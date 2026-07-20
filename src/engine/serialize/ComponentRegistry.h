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
#include <vector>

#include "../Component.h"

namespace ytail {
    // A registered type's serial id and its human-readable name, for editor menus.
    struct ComponentTypeInfo {
        std::string id;
        std::string displayName;
    };

    class ComponentRegistry {
    public:
        // Link a serial id to type T. Pass T::SerialId as the id.
        template<typename T>
        void registerType(const std::string& id) {
            factories[id] = []() -> std::unique_ptr<Component> { return std::make_unique<T>(); };
            types.push_back({ id, T{}.getTypeName() });
        }

        // Build a new component for the given serial id, or nullptr if the id is unknown.
        [[nodiscard]] std::unique_ptr<Component> create(const std::string& id) const;

        // Every registered type, in registration order, for the editor's Add Component menu.
        [[nodiscard]] const std::vector<ComponentTypeInfo>& getTypes() const { return types; }

        // Register every built-in component. Called once at startup so there's one clear list.
        void registerBuiltins();

    private:
        std::unordered_map<std::string, std::function<std::unique_ptr<Component>()>> factories;
        std::vector<ComponentTypeInfo> types;
    };
} // ytail

#endif //YELLOWTAIL_COMPONENTREGISTRY_H
