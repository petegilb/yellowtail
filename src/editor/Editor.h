//
// Created by PeterPC on 7/14/2026.
//

#ifndef YELLOWTAIL_EDITOR_H
#define YELLOWTAIL_EDITOR_H

#include <memory>

#include <SDL3/SDL.h>
#include <glm/fwd.hpp>
#include <nlohmann/json_fwd.hpp>

#include "imgui.h"
#include "imguizmo/ImGuizmo.h"

#include "engine/Application.h"

namespace ytail
{
    class Engine;
    class RigidbodyComponent;

    class Editor : public Application {
    public:
        explicit Editor(Engine* inEngine);
        ~Editor() override;

        void start() override;
        void eventTick(const SDL_Event& event) override;
        void tick(float deltaTime) override;
        void uiTick() override;

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

        // pick the nearest mesh under a window pixel and select it (0 = nothing hit)
        void selectAtScreen(float screenX, float screenY);

        // change selection in the editor (changes outline too)
        void setSelected(Uint32 id);

        // transform gizmo for the current selection
        void drawGizmo();

        // "Entity / Collider N" picker for what the gizmo edits on a rigidbody
        void drawColliderGizmoTarget(RigidbodyComponent* rb);

        // id of the entity shown in the inspector; 0 = none (ids start at 1)
        Uint32 selectedEntity = 0;

        // which collider the gizmo edits on the selection; -1 = the entity transform
        int selectedCollider = -1;

        ImGuizmo::OPERATION gizmoOperation = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;

        // snap step for gizmos
        bool gizmoSnap = false;
        float snapTranslate = 1.0f;
        float snapRotateDegrees = 15.0f;
        float snapScale = 0.1f;
    };
} // ytail

#endif //YELLOWTAIL_EDITOR_H
