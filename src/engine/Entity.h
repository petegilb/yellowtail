//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_ENTITY_H
#define YELLOWTAIL_ENTITY_H
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include "Component.h"

namespace ytail {
    // Generational entity handle. Low 24 bits: slot index. High 8 bits: generation, bumped when
    // a slot is reused so stale ids fail lookup instead of aliasing the new occupant. Slot 0 is
    // reserved, so 0 is never a live id and NULL_ENTITY doubles as "no entity".
    using EntityId = Uint32;

    inline constexpr EntityId NULL_ENTITY = 0;
    inline constexpr Uint32 ENTITY_INDEX_BITS = 24;
    inline constexpr Uint32 ENTITY_INDEX_MASK = (1u << ENTITY_INDEX_BITS) - 1u;
    inline constexpr Uint32 ENTITY_GENERATION_MASK = 0xFFu;

    [[nodiscard]] constexpr Uint32 entityIndex(const EntityId id) { return id & ENTITY_INDEX_MASK; }
    [[nodiscard]] constexpr Uint32 entityGeneration(const EntityId id) { return id >> ENTITY_INDEX_BITS; }
    [[nodiscard]] constexpr EntityId makeEntityId(const Uint32 index, const Uint32 generation) {
        return ((generation & ENTITY_GENERATION_MASK) << ENTITY_INDEX_BITS) | (index & ENTITY_INDEX_MASK);
    }

    class World;

    // Lives by value in World's dense array: pointers to an Entity are transient and go stale on
    // any add/remove. Hold an EntityId and re-fetch instead.
    class Entity {
public:
        Entity(EntityId newId, World* inWorld);
        ~Entity() = default;

        // Moves patch each component's owner pointer, since the vector relocates entities.
        Entity(Entity&& other) noexcept;
        Entity& operator=(Entity&& other) noexcept;
        Entity(const Entity&) = delete;
        Entity& operator=(const Entity&) = delete;

        [[nodiscard]] EntityId getId() const {return entityId;}

        [[nodiscard]] const std::string& getName() const { return name; }
        void setName(const std::string& newName) { name = newName; }

        // Parent resolved through the world (transient pointer); links are stored as ids.
        [[nodiscard]] Entity* getParent() const;
        [[nodiscard]] EntityId getParentId() const { return parentId; }
        [[nodiscard]] const std::vector<EntityId>& getChildIds() const { return childIds; }

        // False keeps this entity out of saved scenes
        [[nodiscard]] bool isSerializable() const { return serializable; }
        void setSerializable(bool value) { serializable = value; }

        // Components in attach order, for the editor to iterate and inspect
        [[nodiscard]] const std::vector<std::unique_ptr<Component>>& getComponents() const { return components; }

        // Fan the fixed/variable ticks out to every attached component.
        void fixedTick(float deltaTime) {
            for (const auto& comp : components) comp->fixedTick(deltaTime);
        }
        void tick(float deltaTime) {
            for (const auto& comp : components) comp->tick(deltaTime);
        }
        void eventTick(const SDL_Event& event){
            for (const auto& comp : components) comp->eventTick(event);
        }

        // creates a new component and attaches it to this entity
        template<typename T, typename... Args>
        T* addComponent(Args&&... args) {
            static_assert(std::is_base_of_v<Component, T>,
                          "T must derive from ytail::Component");
            auto owned = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = owned.get();
            addComponent(std::move(owned));   // one place does the actual attaching
            return ptr;
        }

        // Attach a component that's already been created (e.g. by the load registry).
        Component* addComponent(std::unique_ptr<Component> component) {
            component->owner = this;
            Component* ptr = component.get();
            components.push_back(std::move(component));
            return ptr;
        }

        // Detach and destroy a component previously returned by getComponents(). No-op if it
        // isn't attached to this entity.
        void removeComponent(Component* component) {
            for (auto it = components.begin(); it != components.end(); ++it) {
                if (it->get() == component) {
                    components.erase(it);
                    return;
                }
            }
        }

        // First attached component of type T, or nullptr if the entity has none.
        template<typename T>
        T* getComponent() const {
            for (const auto& c : components) {
                if (T* hit = dynamic_cast<T*>(c.get())) {
                    return hit;
                }
            }
            return nullptr;
        }

        template<typename T>
        [[nodiscard]] bool hasComponent() const {
            return getComponent<T>() != nullptr;
        }

private:
        friend class World;
        World* world = nullptr;
        EntityId parentId = NULL_ENTITY;
        std::vector<EntityId> childIds;

        EntityId entityId = NULL_ENTITY;
        std::string name;
        bool serializable = true;
        // TODO phase 2: components move into per-type pools in the World
        std::vector<std::unique_ptr<Component>> components;
    };
} // ytail

#endif //YELLOWTAIL_ENTITY_H
