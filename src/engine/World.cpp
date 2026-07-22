#include "World.h"

#include <SDL3/SDL.h>

namespace ytail {
    World::World() {
        slots.emplace_back(); // slot 0 reserved so NULL_ENTITY never resolves
    }

    Entity* World::createAt(const Uint32 index) {
        Slot& slot = slots[index];
        slot.denseIndex = static_cast<Uint32>(dense.size());
        dense.emplace_back(makeEntityId(index, slot.generation), this);
        return &dense.back();
    }

    Entity* World::addEntity() {
        Uint32 index;
        if (!freeIndices.empty()) {
            index = freeIndices.back();
            freeIndices.pop_back();
        } else {
            index = static_cast<Uint32>(slots.size());
            slots.emplace_back();
        }
        return createAt(index);
    }

    Entity* World::addEntityWithId(const EntityId id) {
        // Loading runs on a cleared world, so no free-list entry can alias a claimed slot.
        SDL_assert(freeIndices.empty() && "addEntityWithId on a non-fresh world");
        const Uint32 index = entityIndex(id);
        if (index == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Entity id %u has reserved index 0; refusing", id);
            return nullptr;
        }
        if (index >= slots.size()) slots.resize(index + 1);
        Slot& slot = slots[index];
        if (slot.denseIndex != NPOS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Entity id %u already exists; refusing duplicate", id);
            return nullptr;
        }
        slot.generation = entityGeneration(id);
        return createAt(index);
    }

    Entity* World::getEntity(const EntityId id) {
        const Uint32 index = entityIndex(id);
        if (index == 0 || index >= slots.size()) return nullptr;
        const Slot& slot = slots[index];
        if (slot.denseIndex == NPOS || slot.generation != entityGeneration(id)) return nullptr;
        return &dense[slot.denseIndex];
    }

    const Entity* World::getEntity(const EntityId id) const {
        return const_cast<World*>(this)->getEntity(id);
    }

    void World::unlinkFromParent(const Entity* child) {
        Entity* parent = getEntity(child->parentId);
        if (parent == nullptr) return;
        auto& siblings = parent->childIds;
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (*it == child->getId()) {
                siblings.erase(it);
                break;
            }
        }
    }

    bool World::reparent(const EntityId childId, const EntityId parentId) {
        Entity* child = getEntity(childId);
        if (child == nullptr) return false;

        if (parentId != NULL_ENTITY) {
            if (childId == parentId) return false;
            const Entity* newParent = getEntity(parentId);
            if (newParent == nullptr) return false;
            // Walk up from the prospective parent; if we reach child, this would form a cycle.
            for (const Entity* ancestor = newParent; ancestor != nullptr;
                 ancestor = getEntity(ancestor->parentId)) {
                if (ancestor == child) return false;
            }
        }

        unlinkFromParent(child);
        child->parentId = parentId;
        if (parentId != NULL_ENTITY) getEntity(parentId)->childIds.push_back(childId);
        return true;
    }

    void World::removeSingle(const EntityId id) {
        const Uint32 index = entityIndex(id);
        Slot& slot = slots[index];
        const Uint32 denseIdx = slot.denseIndex;

        // Swap-and-pop keeps dense packed; redirect the moved entity's slot to its new spot.
        const Uint32 lastIdx = static_cast<Uint32>(dense.size()) - 1;
        if (denseIdx != lastIdx) {
            dense[denseIdx] = std::move(dense[lastIdx]);
            slots[entityIndex(dense[denseIdx].getId())].denseIndex = denseIdx;
        }
        dense.pop_back();

        slot.denseIndex = NPOS;
        slot.generation = (slot.generation + 1) & ENTITY_GENERATION_MASK; // stale ids now fail lookup
        freeIndices.push_back(index);
    }

    void World::removeEntity(const EntityId id) {
        Entity* entity = getEntity(id);
        if (entity == nullptr) return;

        unlinkFromParent(entity);
        entity->parentId = NULL_ENTITY;

        // Gather the subtree (this id plus every descendant) before erasing anything.
        std::vector<EntityId> toRemove{ id };
        for (size_t i = 0; i < toRemove.size(); ++i) {
            const Entity* current = getEntity(toRemove[i]);
            if (current == nullptr) continue;
            for (const EntityId childId : current->childIds) toRemove.push_back(childId);
        }

        for (const EntityId removeId : toRemove) removeSingle(removeId);
    }

    void World::clear() {
        dense.clear();
        slots.assign(1, {}); // slot 0 stays reserved
        freeIndices.clear();
    }
} // ytail
