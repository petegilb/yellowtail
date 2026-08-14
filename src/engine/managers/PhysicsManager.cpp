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
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>

#include <glm/geometric.hpp>

#include "../render/JoltDebugRenderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <thread>

using namespace JPH;

namespace {
    // Object layers: which broad groups of objects exist. Determines what collides with what.
    namespace Layers {
        static constexpr ObjectLayer NON_MOVING = 0;
        static constexpr ObjectLayer MOVING = 1;
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
            objectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
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
                case Layers::NON_MOVING: return inObject2 == Layers::MOVING; // static only collides with movers
                case Layers::MOVING:     return true; // movers collide with everything
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
        return true; // true = trigger a breakpoint
    }
#endif

    // Sizing limits for the physics world. Bump these if you exceed them.
    constexpr uint cMaxBodies = 1024;
    constexpr uint cNumBodyMutexes = 0; // 0 = let Jolt pick a default
    constexpr uint cMaxBodyPairs = 1024;
    constexpr uint cMaxContactConstraints = 1024;

    // glm <-> Jolt conversions for the body API.
    Vec3 toJolt(const glm::vec3& v)  { return Vec3(v.x, v.y, v.z); }
    Quat toJolt(const glm::quat& q)  { return Quat(q.x, q.y, q.z, q.w); }
    glm::vec3 toGlm(RVec3Arg v)      { return { v.GetX(), v.GetY(), v.GetZ() }; }
    glm::quat toGlm(QuatArg q)       { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

    EMotionType toJoltMotionType(ytail::physics::BodyType type) {
        using namespace ytail::physics;
        switch (type) {
            case BodyType::Dynamic:   return EMotionType::Dynamic;
            case BodyType::Kinematic: return EMotionType::Kinematic;
            case BodyType::Static:
            default:                  return EMotionType::Static;
        }
    }

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
    // Read once, when the world is built. Zero workers means Jolt runs its jobs on the calling
    // thread while it waits on a barrier, which is what makes a run reproducible.
    bool singleThreadedPhysics = false;

    int physicsWorkerThreads() {
        if (singleThreadedPhysics) return 0;
        // Leave one core for the main thread.
        return std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
    }

    struct PhysicsManager::Impl {
        TempAllocatorImpl tempAllocator{ 10 * 1024 * 1024 }; // 10 MiB scratch for the solver
        JobSystemThreadPool jobSystem{ cMaxPhysicsJobs, cMaxPhysicsBarriers, physicsWorkerThreads() };

        BPLayerInterfaceImpl broadPhaseLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
        ObjectLayerPairFilterImpl objectVsObjectLayerFilter;

        PhysicsSystem physicsSystem;

        JoltDebugRenderer debugRenderer;

        // Reused rather than reallocated: one of these is written every fixed tick.
        std::array<StateRecorderImpl, PhysicsManager::MaxSavedContacts> savedContacts;

        void invalidateSavedContacts() {
            for (StateRecorderImpl& recorder : savedContacts) recorder.Clear();
        }
    };

    PhysicsManager& PhysicsManager::get() {
        static PhysicsManager instance;
        return instance;
    }

    void PhysicsManager::setSingleThreaded(const bool singleThreaded) {
        singleThreadedPhysics = singleThreaded;
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

        const bool movable = def.type != BodyType::Static;
        BodyCreationSettings settings(shape, toJolt(def.position), toJolt(def.rotation),
            toJoltMotionType(def.type), movable ? Layers::MOVING : Layers::NON_MOVING);
        settings.mAllowDynamicOrKinematic = movable;

        const BodyProperties& properties = def.properties;
        settings.mFriction = properties.friction;
        settings.mRestitution = properties.restitution;
        settings.mLinearDamping = properties.linearDamping;
        settings.mAngularDamping = properties.angularDamping;
        settings.mGravityFactor = properties.gravityFactor;
        settings.mMaxLinearVelocity = properties.maxLinearVelocity;
        settings.mMaxAngularVelocity = properties.maxAngularVelocity;
        settings.mAllowSleeping = properties.allowSleeping;
        if (properties.overrideMass > 0.0f) {
            // CalculateInertia keeps the shape's inertia tensor and rescales it to the new mass,
            // so an overridden ball still rolls like a ball.
            settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = properties.overrideMass;
        }

        const BodyID id = impl->physicsSystem.GetBodyInterface().CreateAndAddBody(
            settings, movable ? EActivation::Activate : EActivation::DontActivate);
        if (id.IsInvalid()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[Jolt] createBody failed (body limit of %u reached?)", cMaxBodies);
            return InvalidBody;
        }
        impl->invalidateSavedContacts();
        return id.GetIndexAndSequenceNumber();
    }

    void PhysicsManager::removeBody(BodyHandle handle) {
        if (handle == InvalidBody) return;
        BodyInterface& bodyInterface = impl->physicsSystem.GetBodyInterface();
        const BodyID id(handle);
        bodyInterface.RemoveBody(id);
        bodyInterface.DestroyBody(id);
        impl->invalidateSavedContacts();
    }

    void PhysicsManager::getBodyTransform(BodyHandle handle, glm::vec3& outPosition, glm::quat& outRotation) const {
        if (handle == InvalidBody) return;
        RVec3 position;
        Quat rotation;
        impl->physicsSystem.GetBodyInterface().GetPositionAndRotation(BodyID(handle), position, rotation);
        outPosition = toGlm(position);
        outRotation = toGlm(rotation);
    }

    void PhysicsManager::setBodyTransform(BodyHandle handle, const glm::vec3& position, const glm::quat& rotation) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().SetPositionAndRotation(
            BodyID(handle), toJolt(position), toJolt(rotation), EActivation::Activate);
    }

    void PhysicsManager::setBodyMotionType(BodyHandle handle, BodyType type) {
        if (handle == InvalidBody) return;
        BodyInterface& bodyInterface = impl->physicsSystem.GetBodyInterface();
        const BodyID id(handle);
        const bool movable = type != BodyType::Static;
        bodyInterface.SetMotionType(id, toJoltMotionType(type),
            movable ? EActivation::Activate : EActivation::DontActivate);
        // NON_MOVING pairs never collide, so the layer has to follow the motion type.
        bodyInterface.SetObjectLayer(id, movable ? Layers::MOVING : Layers::NON_MOVING);
    }

    glm::vec3 PhysicsManager::getLinearVelocity(BodyHandle handle) const {
        if (handle == InvalidBody) return glm::vec3(0.0f);
        return toGlm(impl->physicsSystem.GetBodyInterface().GetLinearVelocity(BodyID(handle)));
    }

    void PhysicsManager::setLinearVelocity(BodyHandle handle, const glm::vec3& velocity) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().SetLinearVelocity(BodyID(handle), toJolt(velocity));
    }

    glm::vec3 PhysicsManager::getAngularVelocity(BodyHandle handle) const {
        if (handle == InvalidBody) return glm::vec3(0.0f);
        return toGlm(impl->physicsSystem.GetBodyInterface().GetAngularVelocity(BodyID(handle)));
    }

    void PhysicsManager::setAngularVelocity(BodyHandle handle, const glm::vec3& velocity) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().SetAngularVelocity(BodyID(handle), toJolt(velocity));
    }

    void PhysicsManager::addForce(BodyHandle handle, const glm::vec3& force) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().AddForce(BodyID(handle), toJolt(force));
    }

    void PhysicsManager::addForceAtPosition(BodyHandle handle, const glm::vec3& force,
                                            const glm::vec3& worldPosition) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().AddForce(BodyID(handle), toJolt(force), toJolt(worldPosition));
    }

    void PhysicsManager::addTorque(BodyHandle handle, const glm::vec3& torque) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().AddTorque(BodyID(handle), toJolt(torque));
    }

    void PhysicsManager::addImpulse(BodyHandle handle, const glm::vec3& impulse) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().AddImpulse(BodyID(handle), toJolt(impulse));
    }

    void PhysicsManager::addAngularImpulse(BodyHandle handle, const glm::vec3& angularImpulse) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().AddAngularImpulse(BodyID(handle), toJolt(angularImpulse));
    }

    bool PhysicsManager::isBodyActive(BodyHandle handle) const {
        if (handle == InvalidBody) return false;
        return impl->physicsSystem.GetBodyInterface().IsActive(BodyID(handle));
    }

    float PhysicsManager::getMass(BodyHandle handle) const {
        if (handle == InvalidBody) return 0.0f;
        const BodyLockRead lock(impl->physicsSystem.GetBodyLockInterface(), BodyID(handle));
        if (!lock.Succeeded()) return 0.0f;
        const Body& body = lock.GetBody();
        if (body.IsStatic()) return 0.0f;
        const float inverseMass = body.GetMotionProperties()->GetInverseMassUnchecked();
        return inverseMass > 0.0f ? 1.0f / inverseMass : 0.0f;
    }

    void PhysicsManager::setLinearDamping(BodyHandle handle, float damping) {
        if (handle == InvalidBody) return;
        const BodyLockWrite lock(impl->physicsSystem.GetBodyLockInterface(), BodyID(handle));
        if (!lock.Succeeded()) return;
        Body& body = lock.GetBody();
        if (body.IsStatic()) return;
        body.GetMotionProperties()->SetLinearDamping(damping);
    }

    void PhysicsManager::setAngularDamping(BodyHandle handle, float damping) {
        if (handle == InvalidBody) return;
        const BodyLockWrite lock(impl->physicsSystem.GetBodyLockInterface(), BodyID(handle));
        if (!lock.Succeeded()) return;
        Body& body = lock.GetBody();
        if (body.IsStatic()) return;
        body.GetMotionProperties()->SetAngularDamping(damping);
    }

    void PhysicsManager::setFriction(BodyHandle handle, float friction) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().SetFriction(BodyID(handle), friction);
    }

    void PhysicsManager::setRestitution(BodyHandle handle, float restitution) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().SetRestitution(BodyID(handle), restitution);
    }

    void PhysicsManager::applyBodyProperties(BodyHandle handle, const BodyProperties& properties) {
        if (handle == InvalidBody) return;
        // One lock and Body's own setters: BodyInterface takes the same per-body lock internally.
        const BodyLockWrite lock(impl->physicsSystem.GetBodyLockInterface(), BodyID(handle));
        if (!lock.Succeeded()) return;
        Body& body = lock.GetBody();
        body.SetFriction(properties.friction);
        body.SetRestitution(properties.restitution);
        if (body.IsStatic()) return;
        body.SetAllowSleeping(properties.allowSleeping);
        MotionProperties* motion = body.GetMotionProperties();
        motion->SetLinearDamping(properties.linearDamping);
        motion->SetAngularDamping(properties.angularDamping);
        motion->SetGravityFactor(properties.gravityFactor);
        motion->SetMaxLinearVelocity(properties.maxLinearVelocity);
        motion->SetMaxAngularVelocity(properties.maxAngularVelocity);
    }

    void PhysicsManager::setGravityFactor(BodyHandle handle, float factor) {
        if (handle == InvalidBody) return;
        impl->physicsSystem.GetBodyInterface().SetGravityFactor(BodyID(handle), factor);
    }

    bool PhysicsManager::castRay(const glm::vec3& origin, const glm::vec3& direction, RayHit& outHit,
                                 BodyHandle ignoreBody) const {
        const RRayCast ray{ toJolt(origin), toJolt(direction) };
        RayCastResult result;
        const IgnoreSingleBodyFilter bodyFilter{ BodyID(ignoreBody) };
        if (!impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, bodyFilter)) return false;

        outHit.body = result.mBodyID.GetIndexAndSequenceNumber();
        outHit.position = toGlm(ray.GetPointOnRay(result.mFraction));
        outHit.distance = result.mFraction * glm::length(direction);

        // The surface normal has to come off the body itself, since the result only carries the
        // sub-shape that was hit.
        const BodyLockRead lock(impl->physicsSystem.GetBodyLockInterface(), result.mBodyID);
        outHit.normal = lock.Succeeded()
            ? toGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, toJolt(outHit.position)))
            : glm::vec3(0.0f);
        return true;
    }

    void PhysicsManager::saveContacts(const int slot) {
        if (slot < 0 || slot >= MaxSavedContacts) return;
        StateRecorderImpl& recorder = impl->savedContacts[slot];
        recorder.Clear();
        impl->physicsSystem.SaveState(recorder, EStateRecorderState::Contacts);
    }

    bool PhysicsManager::restoreContacts(const int slot) {
        if (slot < 0 || slot >= MaxSavedContacts) return false;
        StateRecorderImpl& recorder = impl->savedContacts[slot];
        // Empty means never written, or thrown away because the body set changed. Jolt rebuilds the
        // cache straight from the BodyIDs in the stream without checking they still exist, so a
        // stale slot has to be refused here rather than handed over.
        if (recorder.GetDataSize() == 0) return false;
        // Rewound every time, so one slot can be restored more than once.
        recorder.Rewind();
        impl->physicsSystem.RestoreState(recorder);
        return true;
    }

    void PhysicsManager::debugDraw(const glm::vec3& cameraPosition) {
        impl->debugRenderer.clear();
        impl->debugRenderer.SetCameraPos(RVec3(cameraPosition.x, cameraPosition.y, cameraPosition.z));

        BodyManager::DrawSettings settings;
        settings.mDrawShape = true;
        settings.mDrawShapeWireframe = true; // routes shapes through DrawLine into the debug renderer's line buffer
        impl->physicsSystem.DrawBodies(settings, &impl->debugRenderer);
    }

    const std::vector<JoltDebugVertex>& PhysicsManager::getDebugLines() const {
        return impl->debugRenderer.getLines();
    }
} // ytail::physics
