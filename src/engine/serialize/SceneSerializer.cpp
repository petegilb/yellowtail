//
// Created for scene serialization.
//

#include "SceneSerializer.h"

#include <fstream>
#include <nlohmann/json.hpp>

#include "Archive.h"
#include "GlmJson.h"
#include "ComponentRegistry.h"
#include "../Engine.h"
#include "../Entity.h"
#include "../Component.h"
#include "../managers/ResourceManager.h"

namespace ytail {
    // Bump this when the scene format changes; components gate new fields on ar.version.
    static constexpr int SCENE_VERSION = 1;

    nlohmann::json saveSceneToJson(const Engine& engine) {
        nlohmann::json root;
        root["version"] = SCENE_VERSION;
        root["ambientColor"] = engine.getAmbientColor();
        root["ambientIntensity"] = engine.getAmbientIntensity();

        nlohmann::json entitiesJson = nlohmann::json::array();
        for (const auto& [id, entity] : engine.getEntities()) {
            if (!entity || !entity->isSerializable()) continue;

            nlohmann::json entityJson;
            entityJson["id"] = id;
            entityJson["name"] = entity->getName();
            if (entity->getParent() != nullptr) {
                entityJson["parent"] = entity->getParent()->getId();
            }

            nlohmann::json componentsJson = nlohmann::json::array();
            for (const auto& component : entity->getComponents()) {
                nlohmann::json compJson;
                compJson["type"] = component->serialId();
                Archive ar{ &compJson, false, SCENE_VERSION, engine.getResourceManager() };
                component->serialize(ar);
                componentsJson.push_back(std::move(compJson));
            }
            entityJson["components"] = std::move(componentsJson);
            entitiesJson.push_back(std::move(entityJson));
        }
        root["entities"] = std::move(entitiesJson);
        return root;
    }

    void loadSceneFromJson(Engine& engine, const nlohmann::json& root) {
        engine.clearScene();

        const int version = root.value("version", 1);
        engine.setAmbientColor(root.value("ambientColor", glm::vec3(0.0f)));
        engine.setAmbientIntensity(root.value("ambientIntensity", 1.0f));

        if (!root.contains("entities")) return;

        // Parent links are resolved in a second pass so a child can reference a parent that
        // hasn't been created yet when we reach the child.
        std::vector<std::pair<Uint32, Uint32>> pendingParents;

        for (const auto& entityJson : root.at("entities")) {
            const Uint32 id = entityJson.at("id").get<Uint32>();
            Entity* entity = engine.addEntityWithId(id);
            entity->setName(entityJson.value("name", std::string()));

            if (entityJson.contains("parent")) {
                pendingParents.emplace_back(id, entityJson.at("parent").get<Uint32>());
            }

            if (!entityJson.contains("components")) continue;
            for (const auto& compJson : entityJson.at("components")) {
                const std::string type = compJson.at("type").get<std::string>();
                std::unique_ptr<Component> component = engine.getComponentRegistry().create(type);
                if (!component) continue;

                // Archive reads from compJson; on load it only pulls values out, never edits it.
                Archive ar{ const_cast<nlohmann::json*>(&compJson), true, version, engine.getResourceManager() };
                Component* attached = entity->addComponent(std::move(component));
                attached->serialize(ar);
            }
        }

        for (const auto& [childId, parentId] : pendingParents) {
            engine.reparent(childId, parentId);
        }
    }

    bool saveScene(const Engine& engine, const std::string& path) {
        std::ofstream file(engine.getResourceManager()->resolveAssetPath(path));
        if (!file.is_open()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not write scene file: %s", path.c_str());
            return false;
        }
        file << saveSceneToJson(engine).dump(2);
        return true;
    }

    bool loadScene(Engine& engine, const std::string& path) {
        std::ifstream file(engine.getResourceManager()->resolveAssetPath(path));
        if (!file.is_open()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open scene file: %s", path.c_str());
            return false;
        }
        nlohmann::json root;
        try {
            file >> root;
        } catch (const std::exception& e) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not parse scene %s: %s", path.c_str(), e.what());
            return false;
        }
        loadSceneFromJson(engine, root);
        return true;
    }
} // ytail
