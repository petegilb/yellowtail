//
// Created by Peter Gilbert on 7/14/26.
//

#ifndef YELLOWTAIL_PHYSICSMANAGER_H
#define YELLOWTAIL_PHYSICSMANAGER_H

#include <memory>

namespace ytail {
    // Owns the Jolt physics world: temp allocator, job system, and the PhysicsSystem itself.
    // Jolt's headers are heavy and require a strict include order (Jolt.h first)
    class PhysicsManager {
    public:
        PhysicsManager();
        ~PhysicsManager();

        // Advance the simulation by a fixed dt. Call from the engine's fixed-step loop.
        // collisionSteps is how many sub-steps Jolt runs internally (1 is fine at 60Hz).
        void step(float deltaTime, int collisionSteps = 1);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
} // ytail

#endif //YELLOWTAIL_PHYSICSMANAGER_H
