//
// Created by PeterPC on 7/14/2026.
//

#ifndef YELLOWTAIL_EDITOR_H
#define YELLOWTAIL_EDITOR_H

#include <memory>
#include <string>

#include <SDL3/SDL.h>
#include <glm/fwd.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/Application.h"
#include "EditorUI.h"

namespace ytail
{
    class Engine;

    // The editor application: scene lifecycle, the fly camera, and picking input.
    // All ImGui panels live in EditorUI, which drives the operations exposed here.
    class Editor : public Application {
    public:
        explicit Editor(Engine* inEngine);
        ~Editor() override;

        void start() override;
        void eventTick(const SDL_Event& event) override;
        void tick(float deltaTime) override;
        void uiTick() override;

        // --- Operations the editor UI drives ---
        [[nodiscard]] Engine* getEngine() const { return engine; }
        [[nodiscard]] const std::string& getCurrentScenePath() const { return currentScenePath; }
        // entity id of the editor fly camera, so the UI can refuse to delete it (0 before start).
        [[nodiscard]] Uint32 getEditorCameraId() const { return editorCameraId; }
        // SDL tick (ms) of the last successful scene save this session; 0 = not saved yet.
        [[nodiscard]] Uint64 getLastSaveTick() const { return lastSaveTick; }

        // Load a scene from an assets-relative path: rebuild the fly cam, keep its pose,
        // clear selection, and adopt the path as the current scene.
        void openScene(const std::string& path);
        // Write the current scene back to its path.
        void saveCurrentScene();
        // Write the current scene to a new path and adopt it on success.
        bool saveSceneAs(const std::string& path);

        // Snapshot the live scene and simulate (Play) / restore it and pause (Stop).
        void play();
        void stop();

    protected:
        void handleInput(const SDL_KeyboardEvent& keyboard_event);

        // The editor's own fly camera, recreated after each scene load (it isn't part of the scene).
        void createEditorCamera();
        // Save/restore the fly-cam pose so it stays put across a scene reload.
        bool captureCameraPose(glm::vec3& outPosition, glm::quat& outRotation) const;
        void applyCameraPose(const glm::vec3& position, const glm::quat& rotation);
        // entity id of the editor camera (0 before it's created).
        Uint32 editorCameraId = 0;

        // Scene state captured on Play, restored on Stop so simulation edits can be undone.
        std::unique_ptr<nlohmann::json> playSnapshot;

        // Assets-relative path of the scene currently being edited; target of Save/Reload.
        std::string currentScenePath = "scenes/main.scene.json";
        // When the scene was last saved (SDL ticks, ms); 0 until the first save.
        Uint64 lastSaveTick = 0;

        EditorUI ui;
    };
} // ytail

#endif //YELLOWTAIL_EDITOR_H
