//
// Created by Peter Gilbert on 7/30.26.
//

#ifndef YELLOWTAIL_COMPONENTPOOL_H
#define YELLOWTAIL_COMPONENTPOOL_H

#include <type_traits>
#include <vector>

#include <SDL3/SDL.h>

#include "Component.h"
#include "Entity.h"

namespace ytail {
    // Each component type gets a small number, handed out the first time the type is used
    inline Uint32 nextComponentTypeId = 0;
    template<typename T>
    Uint32 componentTypeId() {
        static const Uint32 id = nextComponentTypeId++;
        return id;
    }

    // True when T has its own tick override. Lets pools skip ticking unimplemented types
    template<typename T>
    inline constexpr bool overridesTick = !std::is_same_v<decltype(&T::tick), void (Component::*)(float)>;
    template<typename T>
    inline constexpr bool overridesFixedTick = !std::is_same_v<decltype(&T::fixedTick), void (Component::*)(float)>;
    template<typename T>
    inline constexpr bool overridesEventTick = !std::is_same_v<decltype(&T::eventTick), void (Component::*)(const SDL_Event&)>;

    // Common interface so the World can hold pools without knowing each component type.
    class IComponentPool {
    public:
        virtual ~IComponentPool() = default;
        virtual void remove(EntityId id) = 0;
        [[nodiscard]] virtual bool has(EntityId id) const = 0;
        virtual void clear() = 0;
        virtual void fixedTickAll(float deltaTime) = 0;
        virtual void tickAll(float deltaTime) = 0;
        virtual void eventTickAll(const SDL_Event& event) = 0;
    };

    // One pool per component type, packed by value.
    // Removal fills the gap with the last element, so there are no holes and pointers that could go stale
    // uses a sparse set architecture: https://skypjack.github.io/2020-08-02-ecs-baf-part-9/
    // https://www.youtube.com/watch?v=yyZMoE1FAJ0
    template<typename T>
    class ComponentPool final : public IComponentPool {
    public:
        static constexpr Uint32 NULL_INDEX = 0xFFFFFFFFu;

        template<typename... Args>
        T& add(const EntityId id, Args&&... args) {
            const Uint32 index = entityIndex(id);
            if (index >= sparse.size()) sparse.resize(index + 1, NULL_INDEX);
            SDL_assert(sparse[index] == NULL_INDEX && "entity already has this component");
            sparse[index] = static_cast<Uint32>(dense.size());
            denseToEntity.push_back(id);
            return dense.emplace_back(std::forward<Args>(args)...);
        }

        [[nodiscard]] T* get(const EntityId id) {
            const Uint32 denseIdx = denseIndexOf(id);
            return denseIdx == NULL_INDEX ? nullptr : &dense[denseIdx];
        }
        [[nodiscard]] const T* get(const EntityId id) const {
            const Uint32 denseIdx = denseIndexOf(id);
            return denseIdx == NULL_INDEX ? nullptr : &dense[denseIdx];
        }

        [[nodiscard]] bool has(const EntityId id) const override { return denseIndexOf(id) != NULL_INDEX; }

        void remove(const EntityId id) override {
            const Uint32 denseIdx = denseIndexOf(id);
            if (denseIdx == NULL_INDEX) return;

            // Fill the gap with the last component and point its sparse entry at the new spot.
            const Uint32 lastIdx = static_cast<Uint32>(dense.size()) - 1;
            if (denseIdx != lastIdx) {
                dense[denseIdx] = std::move(dense[lastIdx]);
                denseToEntity[denseIdx] = denseToEntity[lastIdx];
                sparse[entityIndex(denseToEntity[denseIdx])] = denseIdx;
            }
            dense.pop_back();
            denseToEntity.pop_back();
            sparse[entityIndex(id)] = NULL_INDEX;
        }

        void clear() override {
            dense.clear();
            denseToEntity.clear();
            sparse.clear();
        }

        void fixedTickAll(const float deltaTime) override {
            if constexpr (overridesFixedTick<T>) {
                for (T& comp : dense) comp.fixedTick(deltaTime);
            }
        }
        void tickAll(const float deltaTime) override {
            if constexpr (overridesTick<T>) {
                for (T& comp : dense) comp.tick(deltaTime);
            }
        }
        void eventTickAll(const SDL_Event& event) override {
            if constexpr (overridesEventTick<T>) {
                for (T& comp : dense) comp.eventTick(event);
            }
        }

        // Direct dense access for iteration (World::each drives these).
        [[nodiscard]] size_t size() const { return dense.size(); }
        [[nodiscard]] EntityId entityAt(const size_t denseIdx) const { return denseToEntity[denseIdx]; }
        [[nodiscard]] T& at(const size_t denseIdx) { return dense[denseIdx]; }
        [[nodiscard]] const T& at(const size_t denseIdx) const { return dense[denseIdx]; }

        void reserve(const size_t count) {
            dense.reserve(count);
            denseToEntity.reserve(count);
        }

    private:
        // Where id's component sits in dense, or NULL_INDEX. Also checks the stored id, so an old
        // id whose slot was reused misses instead of hitting the new entity's component.
        [[nodiscard]] Uint32 denseIndexOf(const EntityId id) const {
            const Uint32 index = entityIndex(id);
            if (index >= sparse.size() || sparse[index] == NULL_INDEX) return NULL_INDEX;
            if (denseToEntity[sparse[index]] != id) return NULL_INDEX;
            return sparse[index];
        }
        
        // note: we don't use infinitely growing indices so i don't think we need to add pagination but im still learning
        std::vector<T> dense;
        std::vector<EntityId> denseToEntity;
        std::vector<Uint32> sparse;
    };
} // ytail

#endif //YELLOWTAIL_COMPONENTPOOL_H
