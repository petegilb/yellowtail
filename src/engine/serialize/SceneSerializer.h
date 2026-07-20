//
// Saves and loads a whole scene (all entities + ambient light) as a JSON file.
//

#ifndef YELLOWTAIL_SCENESERIALIZER_H
#define YELLOWTAIL_SCENESERIALIZER_H

#include <string>

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
} // ytail

#endif //YELLOWTAIL_SCENESERIALIZER_H
