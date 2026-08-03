//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_ENTITY_H
#define YELLOWTAIL_ENTITY_H
#include <string>
#include <vector>

#include <SDL3/SDL_stdinc.h>

namespace ytail {
    // Entity handle. Low 24 bits: the slot the entity lives in. High 8 bits: the generation
    // the generation is bumped each time a slot is used, so we can see deletions without pointers
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

    // Sorts by slot index; a raw id compare would order by generation first.
    [[nodiscard]] constexpr bool entityIdLess(const EntityId a, const EntityId b) {
        return entityIndex(a) < entityIndex(b);
    }

    class World;

    // Components now live in the World in per-type pools
    // Entities are stored by value in one packed array which are moved on
    // adds and deletes so storing by Entity* is unsafe (use EntityId!)
    class Entity {
public:
        Entity(EntityId newId, World* inWorld);
        ~Entity() = default;

        Entity(Entity&& other) noexcept = default;
        Entity& operator=(Entity&& other) noexcept = default;
        Entity(const Entity&) = delete;
        Entity& operator=(const Entity&) = delete;

        [[nodiscard]] EntityId getId() const {return entityId;}

        [[nodiscard]] const std::string& getName() const { return name; }
        void setName(const std::string& newName) { name = newName; }

        [[nodiscard]] Entity* getParent() const;
        [[nodiscard]] EntityId getParentId() const { return parentId; }
        [[nodiscard]] const std::vector<EntityId>& getChildIds() const { return childIds; }

        // False keeps this entity out of saved scenes
        [[nodiscard]] bool isSerializable() const { return serializable; }
        void setSerializable(bool value) { serializable = value; }

        template<typename T, typename... Args>
        T* addComponent(Args&&... args);

        // Component of type T on this entity, or nullptr.
        template<typename T>
        T* getComponent() const;

        template<typename T>
        [[nodiscard]] bool hasComponent() const;

private:
        friend class World;
        World* world = nullptr;
        EntityId parentId = NULL_ENTITY;
        std::vector<EntityId> childIds;

        EntityId entityId = NULL_ENTITY;
        std::string name;
        bool serializable = true;
    };
} // ytail

#endif //YELLOWTAIL_ENTITY_H
