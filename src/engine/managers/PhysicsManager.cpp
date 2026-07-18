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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
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

    // glm <-> Jolt conversions for the body API.
    Vec3 toJolt(const glm::vec3& v)  { return Vec3(v.x, v.y, v.z); }
    Quat toJolt(const glm::quat& q)  { return Quat(q.x, q.y, q.z, q.w); }
    glm::vec3 toGlm(RVec3Arg v)      { return { v.GetX(), v.GetY(), v.GetZ() }; }
    glm::quat toGlm(QuatArg q)       { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

    // The bare Jolt shape for one collider, before its local offset is applied.
    ShapeRefC makeShape(const ytail::physics::ColliderDef& c) {
        using namespace ytail::physics;
        switch (c.shape) {
            case ColliderShape::Sphere:  return new SphereShape(c.radius);
            case ColliderShape::Capsule: return new CapsuleShape(c.halfHeight, c.radius);
            case ColliderShape::Box:
            default:                     return new BoxShape(toJolt(c.halfExtents));
        }
    }
}

namespace ytail::physics {
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
    };

    PhysicsManager& PhysicsManager::get() {
        static PhysicsManager instance;
        return instance;
    }

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
    }

    BodyHandle PhysicsManager::createBody(const BodyDef& def) {
        if (def.colliders.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[Jolt] createBody called with no colliders");
            return InvalidBody;
        }

        ShapeRefC shape;
        if (def.colliders.size() == 1) {
            const ColliderDef& c = def.colliders[0];
            ShapeRefC inner = makeShape(c);
            const bool centered = c.offset == glm::vec3(0.0f) && c.rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            shape = centered
                ? inner
                : ShapeRefC(new RotatedTranslatedShape(toJolt(c.offset), toJolt(c.rotation), inner));
        } else {
            // Keep the inner shapes alive until Create() copies them into the compound.
            std::vector<ShapeRefC> inners;
            inners.reserve(def.colliders.size());
            StaticCompoundShapeSettings compound;
            for (const ColliderDef& c : def.colliders) {
                inners.push_back(makeShape(c));
                compound.AddShape(toJolt(c.offset), toJolt(c.rotation), inners.back());
            }
            ShapeSettings::ShapeResult result = compound.Create();
            if (result.HasError()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[Jolt] compound shape error: %s", result.GetError().c_str());
                return InvalidBody;
            }
            shape = result.Get();
        }

        const bool dynamic = def.type == BodyType::Dynamic;
        BodyCreationSettings settings(shape, toJolt(def.position), toJolt(def.rotation),
            dynamic ? EMotionType::Dynamic : EMotionType::Static,
            dynamic ? Layers::MOVING : Layers::NON_MOVING);

        const BodyID id = impl->physicsSystem.GetBodyInterface().CreateAndAddBody(
            settings, dynamic ? EActivation::Activate : EActivation::DontActivate);
        return id.GetIndexAndSequenceNumber();
    }

    void PhysicsManager::removeBody(BodyHandle handle) {
        if (handle == InvalidBody) return;
        BodyInterface& bodyInterface = impl->physicsSystem.GetBodyInterface();
        const BodyID id(handle);
        bodyInterface.RemoveBody(id);
        bodyInterface.DestroyBody(id);
    }

    void PhysicsManager::getBodyTransform(BodyHandle handle, glm::vec3& outPosition, glm::quat& outRotation) const {
        RVec3 position;
        Quat rotation;
        impl->physicsSystem.GetBodyInterface().GetPositionAndRotation(BodyID(handle), position, rotation);
        outPosition = toGlm(position);
        outRotation = toGlm(rotation);
    }

    void PhysicsManager::setBodyTransform(BodyHandle handle, const glm::vec3& position, const glm::quat& rotation) {
        impl->physicsSystem.GetBodyInterface().SetPositionAndRotation(
            BodyID(handle), toJolt(position), toJolt(rotation), EActivation::Activate);
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
} // ytail::physics
