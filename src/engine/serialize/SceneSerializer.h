//
// Saves and loads a whole scene (all entities + ambient light) as a JSON file.
//

#ifndef YELLOWTAIL_SCENESERIALIZER_H
#define YELLOWTAIL_SCENESERIALIZER_H

#include <string>

#include <SDL3/SDL.h>
#include <nlohmann/json_fwd.hpp>

namespace ytail {
    class Engine;

    // In-memory scene <-> json. Used by the file helpers below and by the editor's
    // Play/Stop snapshot (which keeps the json around without touching disk).
    nlohmann::json saveSceneToJson(const Engine& engine);
    void loadSceneFromJson(Engine& engine, const nlohmann::json& root);

    // Write every serializable entity to the .json file at the given assets-relative path.
    bool saveScene(const Engine& engine, const std::string& path);
    // Clear the scene and rebuild it from the .json file at the given assets-relative path.
    bool loadScene(Engine& engine, const std::string& path);

    // Deep-copy an entity (and its whole child subtree) into the live scene via a
    // serialize round-trip, keeping the source's parent. Returns the new root entity's
    // id, or 0 on failure.
    Uint32 duplicateEntity(Engine& engine, Uint32 sourceId);
} // ytail

#endif //YELLOWTAIL_SCENESERIALIZER_H
