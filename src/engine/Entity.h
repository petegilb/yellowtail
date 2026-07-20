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
    class Entity {
public:
        Entity(Uint32 newId);

        [[nodiscard]] Uint32 getId() const {return entityId;}

        [[nodiscard]] const std::string& getName() const { return name; }
        void setName(const std::string& newName) { name = newName; }

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
        virtual void eventTick(const SDL_Event& event){
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
        Uint32 entityId;
        std::string name;
        bool serializable = true;
        std::vector<std::unique_ptr<Component>> components;
    };
} // ytail

#endif //YELLOWTAIL_ENTITY_H