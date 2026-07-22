//
// Owns every entity, by value, in one dense array. Handles are generational EntityIds
// (see Entity.h): slot indices recycle, generations catch stale ids.
//

#ifndef YELLOWTAIL_WORLD_H
#define YELLOWTAIL_WORLD_H

#include <vector>

#include "Entity.h"

namespace ytail {
    class World {
    public:
        World();

        Entity* addEntity();
        // Claim the exact slot encoded in id. Scene loading only: assumes a freshly cleared world.
        Entity* addEntityWithId(EntityId id);

        // Generation-checked lookup; nullptr for stale or unknown ids. The pointer is transient:
        // any add/remove can relocate entities.
        [[nodiscard]] Entity* getEntity(EntityId id);
        [[nodiscard]] const Entity* getEntity(EntityId id) const;

        // Remove an entity and its whole subtree (all descendants go with it).
        void removeEntity(EntityId id);

        // Remove every entity. Used before loading a scene.
        void clear();

        // Parent childId under parentId, keeping both link sides in sync. NULL_ENTITY detaches
        // to root. Returns false (no-op) on unknown ids, self-parenting, or a cycle.
        bool reparent(EntityId childId, EntityId parentId);

        // Dense, tightly packed storage for iteration. Never has holes; order changes on remove.
        [[nodiscard]] std::vector<Entity>& entities() { return dense; }
        [[nodiscard]] const std::vector<Entity>& entities() const { return dense; }

    private:
        static constexpr Uint32 NPOS = 0xFFFFFFFFu;

        // Per-index bookkeeping: where the entity sits in dense (NPOS = dead slot) and the
        // generation a live id must carry to resolve.
        struct Slot {
            Uint32 denseIndex = NPOS;
            Uint32 generation = 0;
        };

        Entity* createAt(Uint32 index);
        // Swap-and-pop one entity out of dense; bumps the slot generation and frees the index.
        void removeSingle(EntityId id);
        // Drop child from its parent's child list (no-op at root).
        void unlinkFromParent(const Entity* child);

        std::vector<Entity> dense;
        std::vector<Slot> slots;        // indexed by entityIndex(id); slot 0 reserved (NULL_ENTITY)
        std::vector<Uint32> freeIndices;
    };
} // ytail

#endif //YELLOWTAIL_WORLD_H
