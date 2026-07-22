//
// Created for scene serialization.
//

#include "SceneSerializer.h"

#include <fstream>
#include <string>

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
        for (const Entity& entity : engine.getEntityList()) {
            if (!entity.isSerializable()) continue;

            nlohmann::json entityJson;
            entityJson["id"] = entity.getId();
            entityJson["name"] = entity.getName();
            if (entity.getParentId() != NULL_ENTITY) {
                entityJson["parent"] = entity.getParentId();
            }

            nlohmann::json componentsJson = nlohmann::json::array();
            for (const auto& component : entity.getComponents()) {
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
            if (entity == nullptr) continue; // duplicate id in the file; already logged
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

    static bool entityNameInUse(const Engine& engine, const std::string& name) {
        for (const Entity& entity : engine.getEntityList()) {
            if (entity.getName() == name) return true;
        }
        return false;
    }

    // Pick the name for a duplicate: increment a trailing number if the name ends in one
    // ("Box 1" -> "Box 2"), otherwise append one ("Box" -> "Box1"), skipping any name that's
    // already taken so duplicates never collide.
    static std::string nextEntityName(const Engine& engine, const std::string& name) {
        size_t digitsStart = name.size();
        while (digitsStart > 0 && std::isdigit(static_cast<unsigned char>(name[digitsStart - 1]))) {
            digitsStart--;
        }

        std::string stem = name.substr(0, digitsStart);
        int start = 1;
        if (digitsStart < name.size()) {
            try {
                start = std::stoi(name.substr(digitsStart)) + 1;
            } catch (const std::exception&) {
                // Digit run too long to parse: fall back to appending to the whole name.
                stem = name;
                start = 1;
            }
        }

        for (int n = start; ; ++n) {
            std::string candidate = stem + std::to_string(n);
            if (!entityNameInUse(engine, candidate)) return candidate;
        }
    }

    // Clone one entity into the scene (components via a save->load round-trip), reparent it
    // under parentId (NULL_ENTITY = root), then recurse into the source's children. Works in
    // ids and snapshots the source up front: addEntity can relocate every entity.
    static EntityId cloneEntityRecursive(Engine& engine, const EntityId srcId, const EntityId parentId) {
        std::string srcName;
        bool srcSerializable = true;
        std::vector<EntityId> srcChildren;
        std::vector<nlohmann::json> compJsons;
        {
            const Entity* src = engine.getEntity(srcId);
            if (src == nullptr) return NULL_ENTITY;
            srcName = src->getName();
            srcSerializable = src->isSerializable();
            srcChildren = src->getChildIds();
            for (const auto& component : src->getComponents()) {
                nlohmann::json compJson;
                compJson["type"] = component->serialId();
                Archive out{ &compJson, false, SCENE_VERSION, engine.getResourceManager() };
                component->serialize(out);
                compJsons.push_back(std::move(compJson));
            }
        } // src goes stale at the addEntity below; everything needed is snapshotted

        Entity* copy = engine.addEntity();
        const EntityId copyId = copy->getId();
        copy->setName(srcName);
        copy->setSerializable(srcSerializable);

        for (nlohmann::json& compJson : compJsons) {
            std::unique_ptr<Component> clone = engine.getComponentRegistry().create(compJson.at("type").get<std::string>());
            if (!clone) continue;
            Component* attached = copy->addComponent(std::move(clone));
            Archive in{ &compJson, true, SCENE_VERSION, engine.getResourceManager() };
            attached->serialize(in);
        }

        if (parentId != NULL_ENTITY) engine.reparent(copyId, parentId);

        for (const EntityId childId : srcChildren) {
            cloneEntityRecursive(engine, childId, copyId); // relocates entities; copy is stale after
        }
        return copyId;
    }

    Uint32 duplicateEntity(Engine& engine, Uint32 sourceId) {
        const Entity* src = engine.getEntity(sourceId);
        if (!src) return 0;

        // Chosen before cloning (the clones don't exist yet, so they can't shadow the name).
        const std::string newName = nextEntityName(engine, src->getName());
        const EntityId parentId = src->getParentId(); // src goes stale once cloning starts
        const EntityId copyId = cloneEntityRecursive(engine, sourceId, parentId);
        if (copyId == NULL_ENTITY) return 0;

        // Only the top-level duplicate is renamed; children keep their names.
        engine.getEntity(copyId)->setName(newName);
        return copyId;
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
