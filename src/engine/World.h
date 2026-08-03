//
// Created by Peter Gilbert on 7/30/26.
//

#ifndef YELLOWTAIL_WORLD_H
#define YELLOWTAIL_WORLD_H

#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "Component.h"
#include "ComponentPool.h"
#include "Entity.h"

namespace ytail {
    class World {
    public:
        World();

        Entity* addEntity();
        // Claim the exact slot encoded in id. Scene loading only: assumes a freshly cleared world.
        Entity* addEntityWithId(EntityId id);

        // Look up an entity; nullptr for deleted or unknown ids. The pointer is only safe until the next add/remove.
        [[nodiscard]] Entity* getEntity(EntityId id);
        [[nodiscard]] const Entity* getEntity(EntityId id) const;

        // Remove an entity, its components, and its whole subtree
        void removeEntity(EntityId id);

        // Remove every entity and component. Used before loading a scene (pools keep capacity).
        void clear();

        // Parent childId under parentId, keeping both link sides in sync. NULL_ENTITY detaches
        // to root. Returns false (no-op) on unknown ids, self-parenting, or a cycle.
        bool reparent(EntityId childId, EntityId parentId);

        // Every live entity, packed for iteration. Order changes when entities are removed.
        [[nodiscard]] std::vector<Entity>& entities() { return dense; }
        [[nodiscard]] const std::vector<Entity>& entities() const { return dense; }

        // Pre-size entity storage (scene load knows the counts up front).
        void reserveEntities(size_t count) {
            dense.reserve(count);
            slots.reserve(count + 1);
        }

        // Pre-size the pool for T (created if needed).
        template<typename T>
        void reservePool(size_t count);

        // ---- components ----

        // Create a component of type T on the entity. The pool for T is created on first use,
        // so new component types need no setup. If the entity already has one, returns it.
        template<typename T, typename... Args>
        T* addComponent(EntityId id, Args&&... args);

        // The entity's component of type T, or nullptr. Cheap: two array reads.
        template<typename T>
        [[nodiscard]] T* get(EntityId id);
        template<typename T>
        [[nodiscard]] const T* get(EntityId id) const;

        template<typename T>
        [[nodiscard]] bool has(EntityId id) const;

        template<typename T>
        void removeComponent(EntityId id);

        // Call func(EntityId, Primary&, Rest&...) for every entity that has all the listed
        // types. Walks Primary's pool and checks the others per entity, so put the rarest
        // type first. Use defer() for any add/remove inside func.
        template<typename Primary, typename... Rest, typename Func>
        void each(Func&& func);
        template<typename Primary, typename... Rest, typename Func>
        void each(Func&& func) const;

        // Tick every component, one pool at a time -> Deferred commands run after each pass.
        void fixedTickAll(float deltaTime);
        void tickAll(float deltaTime);
        void eventTickAll(const SDL_Event& event);

        // Queue work that adds or removes entities/components from inside a tick
        // Runs when the current tick pass finishes (or at the end of the next one if queued outside a tick).
        void defer(std::function<void(World&)> command);
        // Queue removeEntity(id). If the entity is already gone by then, nothing happens.
        void deferDestroy(EntityId id);

    private:
        static constexpr Uint32 NULL_INDEX = 0xFFFFFFFFu;

        // per slot index aka where the entity sits in dense (NULL_INDEX = empty slot)
        // and the generation an id must match to look up.
        struct Slot {
            Uint32 denseIndex = NULL_INDEX;
            Uint32 generation = 0;
        };

        Entity* createAt(Uint32 index);
        // Remove one entity and its components -> bumps the slot's generation and frees the index.
        void removeSingle(EntityId id);
        // Drop child from its parent's child list (no-op at root).
        void unlinkFromParent(const Entity* child);
        // Run queued commands until none remain (a command may queue more).
        void flushDeferred();

        template<typename T>
        ComponentPool<T>& ensurePool();
        template<typename T>
        [[nodiscard]] ComponentPool<T>* tryGetPool() const;

        // another sparse set implementation similar to ComponentPool
        // instead of a sparse array, we use an array of slots which includes the index + generation
        std::vector<Entity> dense;
        std::vector<Slot> slots;
        std::vector<Uint32> freeIndices;

        // indexed by componentTypeId
        std::vector<std::unique_ptr<IComponentPool>> pools;
        // Nonzero while ticks/each are walking the pools (assert using this)
        mutable int iterationDepth = 0;

        // Queued defer() commands, plus a scratch list so flushing reuses its memory each frame.
        std::vector<std::function<void(World&)>> pendingCommands;
        std::vector<std::function<void(World&)>> commandScratch;
    };

    // ---- template definitions ----

    template<typename T>
    ComponentPool<T>& World::ensurePool() {
        const Uint32 typeId = componentTypeId<T>();
        if (typeId >= pools.size()) pools.resize(typeId + 1);
        if (!pools[typeId]) pools[typeId] = std::make_unique<ComponentPool<T>>();
        return static_cast<ComponentPool<T>&>(*pools[typeId]);
    }

    template<typename T>
    ComponentPool<T>* World::tryGetPool() const {
        const Uint32 typeId = componentTypeId<T>();
        if (typeId >= pools.size() || !pools[typeId]) return nullptr;
        return static_cast<ComponentPool<T>*>(pools[typeId].get());
    }

    template<typename T, typename... Args>
    T* World::addComponent(const EntityId id, Args&&... args) {
        static_assert(std::is_base_of_v<Component, T>, "T must derive from ytail::Component");
        SDL_assert(iterationDepth == 0 && "component add during iteration; defer to after the loop");
        if (getEntity(id) == nullptr) return nullptr;

        ComponentPool<T>& pool = ensurePool<T>();
        if (T* existing = pool.get(id)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Entity %u already has a %s; returning it",
                        id, existing->getTypeName());
            return existing;
        }
        T& comp = pool.add(id, std::forward<Args>(args)...);
        comp.ownerId = id;
        comp.world = this;
        return &comp;
    }

    template<typename T>
    T* World::get(const EntityId id) {
        ComponentPool<T>* pool = tryGetPool<T>();
        return pool != nullptr ? pool->get(id) : nullptr;
    }

    template<typename T>
    const T* World::get(const EntityId id) const {
        const ComponentPool<T>* pool = tryGetPool<T>();
        return pool != nullptr ? pool->get(id) : nullptr;
    }

    template<typename T>
    bool World::has(const EntityId id) const {
        const ComponentPool<T>* pool = tryGetPool<T>();
        return pool != nullptr && pool->has(id);
    }

    template<typename T>
    void World::removeComponent(const EntityId id) {
        SDL_assert(iterationDepth == 0 && "component remove during iteration; defer to after the loop");
        if (ComponentPool<T>* pool = tryGetPool<T>()) pool->remove(id);
    }

    template<typename T>
    void World::reservePool(const size_t count) {
        ensurePool<T>().reserve(count);
    }

    template<typename Primary, typename... Rest, typename Func>
    void World::each(Func&& func) {
        ComponentPool<Primary>* primary = tryGetPool<Primary>();
        if (primary == nullptr) return;
        // Look the extra pools up once: if any is missing nothing can match, and the per-entity
        // checks stay two array reads instead of a pool lookup each.
        const std::tuple<ComponentPool<Rest>*...> restPools{ tryGetPool<Rest>()... };
        if (!(... && (std::get<ComponentPool<Rest>*>(restPools) != nullptr))) return;
        ++iterationDepth;
        for (size_t i = 0; i < primary->size(); ++i) {
            const EntityId id = primary->entityAt(i);
            const std::tuple<Rest*...> rest{ std::get<ComponentPool<Rest>*>(restPools)->get(id)... };
            if ((... && (std::get<Rest*>(rest) != nullptr))) {
                func(id, primary->at(i), *std::get<Rest*>(rest)...);
            }
        }
        --iterationDepth;
    }

    template<typename Primary, typename... Rest, typename Func>
    void World::each(Func&& func) const {
        const ComponentPool<Primary>* primary = tryGetPool<Primary>();
        if (primary == nullptr) return;
        const std::tuple<const ComponentPool<Rest>*...> restPools{ tryGetPool<Rest>()... };
        if (!(... && (std::get<const ComponentPool<Rest>*>(restPools) != nullptr))) return;
        ++iterationDepth;
        for (size_t i = 0; i < primary->size(); ++i) {
            const EntityId id = primary->entityAt(i);
            const std::tuple<const Rest*...> rest{ std::get<const ComponentPool<Rest>*>(restPools)->get(id)... };
            if ((... && (std::get<const Rest*>(rest) != nullptr))) {
                func(id, primary->at(i), *std::get<const Rest*>(rest)...);
            }
        }
        --iterationDepth;
    }

    // Entity's component API delegates to the world's pools.
    template<typename T, typename... Args>
    T* Entity::addComponent(Args&&... args) {
        return world->addComponent<T>(entityId, std::forward<Args>(args)...);
    }

    template<typename T>
    T* Entity::getComponent() const {
        return world->get<T>(entityId);
    }

    template<typename T>
    bool Entity::hasComponent() const {
        return getComponent<T>() != nullptr;
    }

    // Component's sibling lookup, same delegation.
    template<typename T>
    T* Component::getSibling() const {
        return world != nullptr ? world->get<T>(ownerId) : nullptr;
    }
} // ytail

#endif //YELLOWTAIL_WORLD_H
