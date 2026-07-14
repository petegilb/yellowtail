//
// Created by Peter Gilbert on 7/14/26.
//
// Jolt integration boilerplate, following the v5.5.0 HelloWorld sample:
// https://github.com/jrouwe/JoltPhysics/blob/master/HelloWorld/HelloWorld.cpp

#include "../managers/PhysicsManager.h"

// Jolt.h must be the first Jolt include.
#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyManager.h>

#include "../render/JoltDebugRenderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <thread>

using namespace JPH;

namespace {
    // Object layers: which broad groups of objects exist. Determines what collides with what.
    namespace Layers {
        static constexpr ObjectLayer NON_MOVING = 0;
        static constexpr ObjectLayer MOVING     = 1;
        static constexpr ObjectLayer NUM_LAYERS = 2;
    }

    // Broadphase layers: a coarser bucketing used to accelerate the broadphase. Static geometry
    // lives apart from movers so the broadphase can skip static-vs-static pairs.
    namespace BroadPhaseLayers {
        static constexpr BroadPhaseLayer NON_MOVING(0);
        static constexpr BroadPhaseLayer MOVING(1);
        static constexpr uint NUM_LAYERS(2);
    }

    // Maps each object layer to a broadphase layer.
    class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
    public:
        BPLayerInterfaceImpl() {
            objectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
            objectToBroadPhase[Layers::MOVING]     = BroadPhaseLayers::MOVING;
        }

        virtual uint GetNumBroadPhaseLayers() const override {
            return BroadPhaseLayers::NUM_LAYERS;
        }

        virtual BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
            JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
            return objectToBroadPhase[inLayer];
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        virtual const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
            switch ((BroadPhaseLayer::Type)inLayer) {
                case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
                case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:     return "MOVING";
                default: JPH_ASSERT(false); return "INVALID";
            }
        }
#endif

    private:
        BroadPhaseLayer objectToBroadPhase[Layers::NUM_LAYERS];
    };

    // Object layer vs broadphase layer: coarse test used to reject pairs early.
    class ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter {
    public:
        virtual bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
            switch (inLayer1) {
                case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
                case Layers::MOVING:     return true;
                default: JPH_ASSERT(false); return false;
            }
        }
    };

    // Object layer vs object layer: the fine test for whether two bodies can collide.
    class ObjectLayerPairFilterImpl : public ObjectLayerPairFilter {
    public:
        virtual bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override {
            switch (inObject1) {
                case Layers::NON_MOVING: return inObject2 == Layers::MOVING;  // static only collides with movers
                case Layers::MOVING:     return true;                          // movers collide with everything
                default: JPH_ASSERT(false); return false;
            }
        }
    };

    // Route Jolt's trace + assert output through SDL logging.
    void TraceImpl(const char* inFMT, ...) {
        va_list list;
        va_start(list, inFMT);
        char buffer[1024];
        SDL_vsnprintf(buffer, sizeof(buffer), inFMT, list);
        va_end(list);
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[Jolt] %s", buffer);
    }

#ifdef JPH_ENABLE_ASSERTS
    bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint inLine) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[Jolt] %s:%u: (%s) %s",
            inFile, inLine, inExpression, inMessage != nullptr ? inMessage : "");
        return true;  // true = trigger a breakpoint
    }
#endif

    // Sizing limits for the physics world. Bump these if you exceed them.
    constexpr uint cMaxBodies             = 1024;
    constexpr uint cNumBodyMutexes        = 0;     // 0 = let Jolt pick a default
    constexpr uint cMaxBodyPairs          = 1024;
    constexpr uint cMaxContactConstraints = 1024;
}

namespace ytail {
    struct PhysicsManager::Impl {
        TempAllocatorImpl tempAllocator{ 10 * 1024 * 1024 };  // 10 MiB scratch for the solver
        JobSystemThreadPool jobSystem{
            cMaxPhysicsJobs, cMaxPhysicsBarriers,
            std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1)  // leave one core for the main thread
        };

        BPLayerInterfaceImpl broadPhaseLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
        ObjectLayerPairFilterImpl objectVsObjectLayerFilter;

        PhysicsSystem physicsSystem;

        JoltDebugRenderer debugRenderer;

        // TODO --- temporary self-test: a falling sphere over a static floor (delete once we have a RigidbodyComponent) ---
        BodyID sphereId;
        float lastLoggedY = 1e9f;
    };

    PhysicsManager::PhysicsManager() {
        // Process-global Jolt setup. Must happen before any allocations/bodies, and only once
        // per process - so keep a single PhysicsManager alive at a time.
        RegisterDefaultAllocator();
        Trace = TraceImpl;
        JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)
        Factory::sInstance = new Factory();
        RegisterTypes();

        // Now the allocator is registered, so the Impl members (temp allocator, job system) can construct.
        impl = std::make_unique<Impl>();

        impl->physicsSystem.Init(
            cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
            impl->broadPhaseLayerInterface,
            impl->objectVsBroadPhaseLayerFilter,
            impl->objectVsObjectLayerFilter);

        // TODO --- temporary self-test scene ---
        BodyInterface& bodyInterface = impl->physicsSystem.GetBodyInterface();

        BoxShapeSettings floorShapeSettings(Vec3(100.0f, 1.0f, 100.0f));
        floorShapeSettings.SetEmbedded();  // lives on the stack; don't let Jolt free it
        ShapeRefC floorShape = floorShapeSettings.Create().Get();
        BodyCreationSettings floorSettings(floorShape, RVec3(0.0f, -1.0f, 0.0f),
            Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING);
        bodyInterface.CreateAndAddBody(floorSettings, EActivation::DontActivate);

        BodyCreationSettings sphereSettings(new SphereShape(0.5f), RVec3(0.0f, 5.0f, 0.0f),
            Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
        impl->sphereId = bodyInterface.CreateAndAddBody(sphereSettings, EActivation::Activate);

        // Rebuild the broadphase after bulk-adding bodies (recommended once at startup).
        impl->physicsSystem.OptimizeBroadPhase();

        SDL_Log("[Jolt] PhysicsManager initialized (%d worker threads)", impl->jobSystem.GetMaxConcurrency() - 1);
    }

    PhysicsManager::~PhysicsManager() {
        // Destroy the world (PhysicsSystem, job system, allocator) before tearing down the
        // global factory/type registry, since bodies reference registered types.
        impl.reset();

        UnregisterTypes();
        delete Factory::sInstance;
        Factory::sInstance = nullptr;
    }

    void PhysicsManager::step(float deltaTime, int collisionSteps) {
        const EPhysicsUpdateError err =
            impl->physicsSystem.Update(deltaTime, collisionSteps, &impl->tempAllocator, &impl->jobSystem);
        if (err != EPhysicsUpdateError::None) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[Jolt] physics update error: %d", static_cast<int>(err));
        }

        // --- temporary self-test: log the sphere's height as it falls and settles ---
        const RVec3 pos = impl->physicsSystem.GetBodyInterface().GetCenterOfMassPosition(impl->sphereId);
        const float y = static_cast<float>(pos.GetY());
        if (SDL_fabsf(y - impl->lastLoggedY) > 0.01f) {
            SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[Jolt] sphere Y = %.2f", y);
            impl->lastLoggedY = y;
        }
    }

    void PhysicsManager::debugDraw() {
        impl->debugRenderer.clear();

        BodyManager::DrawSettings settings;
        settings.mDrawShape = true;
        settings.mDrawShapeWireframe = true;  // routes shapes through DrawLine into the debug renderer's line buffer
        impl->physicsSystem.DrawBodies(settings, &impl->debugRenderer);
    }

    const std::vector<JoltDebugVertex>& PhysicsManager::getDebugLines() const {
        return impl->debugRenderer.getLines();
    }
} // ytail
