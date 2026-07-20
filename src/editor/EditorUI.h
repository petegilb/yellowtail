//
// ImGui-driven editor panels (menu bar, toolbar, outliner, inspector, gizmo,
// material editor). Owns all UI-only + interaction state (selection, gizmo
// settings, material authoring, dialogs) and drives the owning Editor for
// scene-level operations (open/save/play).
//

#ifndef YELLOWTAIL_EDITORUI_H
#define YELLOWTAIL_EDITORUI_H

#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/fwd.hpp>

#include "imgui.h"
#include "imguizmo/ImGuizmo.h"

#include "engine/render/Material.h"

namespace ytail {
    class Engine;
    class Editor;
    class Entity;
    class RigidbodyComponent;
    class RenderComponent;

    // One texture entry while authoring a material: either a solid color or a file path.
    struct MaterialTextureDef {
        bool solid = true;
        glm::vec4 color{1.0f};
        std::string path;
        bool srgb = false;
        SamplerType sampler = SamplerType::LinearWrap;
    };

    // Editor-side authoring model for a .mat file (the file is the source of truth).
    struct MaterialDef {
        PipelineType pipeline = PipelineType::LitStatic;
        std::vector<MaterialTextureDef> textures;
        MaterialUniform uniform;
    };

    class EditorUI {
    public:
        explicit EditorUI(Editor* editor);

        // Build every panel for the frame; called from Editor::uiTick.
        void draw();

        // Left-click in the 3D scene: pick the entity under the cursor (unless the click
        // belongs to ImGui or the gizmo). Forwarded from Editor::eventTick.
        void handleSceneClick(float screenX, float screenY);
        // Gizmo-mode hotkeys (W/E/R). Forwarded from Editor::handleInput.
        void handleKey(const SDL_KeyboardEvent& keyboard_event);

        // change selection in the editor (updates the outline highlight too)
        void setSelected(Uint32 id);
        [[nodiscard]] Uint32 getSelectedEntity() const { return selectedEntity; }

    private:
        // panels
        void drawMenuBar();
        void drawToolbar();
        void drawOutliner();
        void drawInspector();
        void drawSaveAsDialog();

        // one entity row in the outliner tree, recursing into its children
        void drawOutlinerNode(Entity* entity);
        // inspector combo to set the selected entity's parent
        void drawParentSelector(Entity* entity);
        // mesh + material slot pickers shown under a RenderComponent in the inspector
        void drawRenderComponentAssets(RenderComponent* render);
        // "Entity / Collider N" picker for what the gizmo edits on a rigidbody
        void drawColliderGizmoTarget(RigidbodyComponent* rb);

        // floating window to author and save .mat files
        void drawMaterialEditor();
        void saveMaterialDef();
        void loadMaterialDef();

        // transform gizmo for the current selection
        void drawGizmo();

        // pick the nearest mesh under a window pixel and select it (0 = nothing hit)
        void selectAtScreen(float screenX, float screenY);

        // Filenames (not full paths) of every *.scene.json under assets/scenes, sorted.
        [[nodiscard]] std::vector<std::string> listSceneFiles() const;

        Editor* editor = nullptr;
        Engine* engine = nullptr;

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

        // Material editor window state
        bool showMaterialEditor = false;
        MaterialDef materialDef;
        std::string materialPath = "materials/new.mat";

        // Save As modal state: request flag (open next frame) + the typed filename.
        bool openSaveAsRequested = false;
        std::string saveAsName;
    };
} // ytail

#endif //YELLOWTAIL_EDITORUI_H
