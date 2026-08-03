//
// Maps each component's serialId to functions that work on its pool, so generic code (the
// scene loader, the editor inspector, entity cloning) can handle any registered type without
// knowing the concrete class.
//

#ifndef YELLOWTAIL_COMPONENTREGISTRY_H
#define YELLOWTAIL_COMPONENTREGISTRY_H

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../World.h"

namespace ytail {
    // A registered type's serial id, its human-readable name, and its pool operations.
    struct ComponentTypeInfo {
        std::string id;
        std::string displayName;
        std::function<Component*(World&, EntityId)> emplace; // add a fresh one to the entity
        std::function<Component*(World&, EntityId)> get; // nullptr if the entity has none
        std::function<void(World&, EntityId)> remove;
        std::function<void(World&, size_t)> reserve; // pre-size the type's pool
    };

    class ComponentRegistry {
    public:
        // Link a serial id to type T. Pass T::SerialId as the id.
        template<typename T>
        void registerType(const std::string& id) {
            componentTypeId<T>(); // claim the type id now: registration order = tick order
            ComponentTypeInfo info;
            info.id = id;
            info.displayName = T{}.getTypeName();
            info.emplace = [](World& world, const EntityId entity) -> Component* {
                return world.addComponent<T>(entity);
            };
            info.get = [](World& world, const EntityId entity) -> Component* {
                return world.get<T>(entity);
            };
            info.remove = [](World& world, const EntityId entity) {
                world.removeComponent<T>(entity);
            };
            info.reserve = [](World& world, const size_t count) {
                world.reservePool<T>(count);
            };
            indexById[id] = types.size();
            types.push_back(std::move(info));
        }

        // Add a fresh component of the given serial id to the entity. Nullptr if the id is
        // unknown or the entity already has one (one component per type; the first wins).
        Component* emplaceById(const std::string& id, World& world, EntityId entity) const;

        // Pre-size the pool for the given serial id; silently skips unknown ids (the load
        // pass logs them when it tries to emplace).
        void reserveById(const std::string& id, World& world, size_t count) const;

        // Every registered type, in registration order, for the editor's Add Component menu
        // and inspector.
        [[nodiscard]] const std::vector<ComponentTypeInfo>& getTypes() const { return types; }

        // Register every built-in component. Called once at startup so there's one clear list.
        void registerBuiltins();

    private:
        std::unordered_map<std::string, size_t> indexById;
        std::vector<ComponentTypeInfo> types;
    };
} // ytail

#endif //YELLOWTAIL_COMPONENTREGISTRY_H
