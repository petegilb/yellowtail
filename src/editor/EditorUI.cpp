//
// ImGui panels for the editor. See EditorUI.h.
//

#include "EditorUI.h"

#include "Editor.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "imguizmo/ImGuizmo.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "engine/Engine.h"
#include "engine/Entity.h"
#include "engine/Component.h"
#include "engine/Input.h"
#include "engine/components/RenderComponent.h"
#include "engine/components/TransformComponent.h"
#include "engine/components/RigidbodyComponent.h"
#include "engine/render/Mesh.h"
#include "engine/managers/ResourceManager.h"
#include "engine/serialize/ComponentRegistry.h"
#include "engine/serialize/EnumJson.h"
#include "engine/serialize/SceneSerializer.h"

#include <nlohmann/json.hpp>

namespace ytail
{
    EditorUI::EditorUI(Editor* inEditor)
        : editor(inEditor), engine(inEditor->getEngine()) {
    }

    static bool rayAabb(const glm::vec3& origin, const glm::vec3& dir,
                        const glm::vec3& min, const glm::vec3& max, float& tHit) {
        float tmin = 0.0f;
        float tmax = FLT_MAX;
        for (int i = 0; i < 3; ++i) {
            if (SDL_fabsf(dir[i]) < 1e-8f) {
                // ray parallel to this slab: miss if the origin is outside it
                if (origin[i] < min[i] || origin[i] > max[i]) return false;
            } else {
                const float inv = 1.0f / dir[i];
                float t1 = (min[i] - origin[i]) * inv;
                float t2 = (max[i] - origin[i]) * inv;
                if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
                tmin = SDL_max(tmin, t1);
                tmax = SDL_min(tmax, t2);
                if (tmin > tmax) return false;
            }
        }
        tHit = tmin;
        return true;
    }

    void EditorUI::handleSceneClick(float screenX, float screenY){
        // Only trust the gizmo hit-test while something is selected
        const bool overGizmo = selectedEntity != 0 && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
        if (ImGui::GetIO().WantCaptureMouse
            || Input::get().isMouseCaptured()
            || overGizmo) {
            return;
        }
        selectAtScreen(screenX, screenY);
    }

    void EditorUI::handleKey(const SDL_KeyboardEvent& keyboard_event){
        // Gizmo modes. disabled when flying
        if (Input::get().isMouseCaptured() || ImGui::GetIO().WantTextInput) return;
        switch (keyboard_event.key) {
            case SDLK_W: gizmoOperation = ImGuizmo::TRANSLATE; break;
            case SDLK_E: gizmoOperation = ImGuizmo::ROTATE; break;
            case SDLK_R: gizmoOperation = ImGuizmo::SCALE; break;
            default: ;
        }
    }

    void EditorUI::selectAtScreen(float screenX, float screenY){
        glm::vec3 origin, dir;
        if (!engine->screenPointToRay(screenX, screenY, origin, dir)) return;

        Uint32 picked = 0;
        float bestT = FLT_MAX;
        for (const auto& [id, entity] : engine->getEntities()) {
            if (!entity) continue;
            auto* transform = entity->getComponent<TransformComponent>();
            auto* render = entity->getComponent<RenderComponent>();
            if (!transform || !render || !render->mesh) continue;

            // Transform the ray into mesh-local space. localDir is left un-normalized so the hit
            // distance stays in the same units across entities (nearest hit wins).
            const glm::mat4 invModel = glm::inverse(transform->worldMatrix());
            const glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(origin, 1.0f));
            const glm::vec3 localDir    = glm::vec3(invModel * glm::vec4(dir, 0.0f));

            float t;
            if (rayAabb(localOrigin, localDir, render->mesh->aabbMin, render->mesh->aabbMax, t) && t < bestT) {
                bestT = t;
                picked = id;
            }
        }
        setSelected(picked);
    }

    void EditorUI::setSelected(Uint32 id){
        if (id == selectedEntity) return;

        if (Entity* previous = engine->getEntity(selectedEntity)){
            if (auto* renderComp = previous->getComponent<RenderComponent>()){
                renderComp->outline = false;
            }
        }

        selectedEntity = id;
        selectedCollider = -1;
        engine->selectedEntity = id;  // drives the selected point light's attenuation gizmo

        if (Entity* current = engine->getEntity(selectedEntity)){
            if (auto* renderComp = current->getComponent<RenderComponent>()){
                renderComp->outline = true;
            }
        }
    }

    void EditorUI::drawOutlinerNode(Entity* entity) {
        if (entity == nullptr) return;

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entity->getId() == selectedEntity) flags |= ImGuiTreeNodeFlags_Selected;
        if (entity->getChildren().empty()) flags |= ImGuiTreeNodeFlags_Leaf;

        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(entity->getId())),
            flags, "%s", entity->getName().c_str());

        // Selecting the row, but not when the click only toggled the expand arrow.
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            setSelected(entity->getId());
        }

        // Right-click row: select it, then offer entity actions.
        if (ImGui::BeginPopupContextItem()) {
            setSelected(entity->getId());
            if (entity->getId() != editor->getEditorCameraId() && ImGui::MenuItem("Duplicate")) {
                if (const Uint32 copy = duplicateEntity(*engine, entity->getId())) setSelected(copy);
            }
            ImGui::EndPopup();
        }

        if (open) {
            std::vector<Uint32> childIds;
            childIds.reserve(entity->getChildren().size());
            for (Entity* child : entity->getChildren()) childIds.push_back(child->getId());
            std::sort(childIds.begin(), childIds.end());
            for (const Uint32 childId : childIds) drawOutlinerNode(engine->getEntity(childId));
            ImGui::TreePop();
        }
    }

    void EditorUI::drawParentSelector(Entity* entity) {
        // Dynamic bodies are world-authoritative, so we keep them as roots (see RigidbodyComponent).
        if (auto* rigidbody = entity->getComponent<RigidbodyComponent>()) {
            if (rigidbody->type == physics::BodyType::Dynamic) {
                ImGui::BeginDisabled();
                ImGui::LabelText("Parent", "root only (dynamic body)");
                ImGui::EndDisabled();
                return;
            }
        }

        Entity* currentParent = entity->getParent();
        const char* preview = currentParent ? currentParent->getName().c_str() : "(none)";
        if (!ImGui::BeginCombo("Parent", preview)) return;

        if (ImGui::Selectable("(none)", currentParent == nullptr)) {
            engine->reparent(entity->getId(), 0);
        }

        std::vector<Uint32> ids;
        for (const auto& [id, other] : engine->getEntities()) {
            if (id != entity->getId()) ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        for (const Uint32 id : ids) {
            Entity* other = engine->getEntity(id);
            if (!other) continue;
            if (ImGui::Selectable(other->getName().c_str(), other == currentParent)) {
                engine->reparent(entity->getId(), id);
            }
        }
        ImGui::EndCombo();
    }

    void EditorUI::drawRenderComponentAssets(RenderComponent* render) {
        ResourceManager* resources = engine->getResourceManager();

        std::string meshPath = render->mesh ? render->mesh->sourcePath : std::string();
        if (ImGui::InputText("Mesh", &meshPath, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (auto mesh = resources->getMesh(meshPath)) render->setMesh(mesh);
        }

        ImGui::SeparatorText("Materials");
        int removeSlot = -1;
        for (size_t slot = 0; slot < render->materials.size(); ++slot) {
            ImGui::PushID(static_cast<int>(slot));
            const auto& material = render->materials[slot];
            std::string slotPath = material ? material->sourcePath : std::string();
            if (ImGui::InputText("##mat", &slotPath, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (auto loaded = resources->getMaterial(slotPath)) render->materials[slot] = loaded;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) removeSlot = static_cast<int>(slot);
            ImGui::PopID();
        }
        if (removeSlot >= 0) render->materials.erase(render->materials.begin() + removeSlot);
        if (ImGui::SmallButton("Add Material Slot")) {
            // Default new slots to sphere.mat rather than an empty slot.
            render->materials.push_back(resources->getMaterial("materials/sphere.mat"));
        }
    }

    void EditorUI::drawMaterialEditor() {
        if (!showMaterialEditor) return;
        if (!ImGui::Begin("Material Editor", &showMaterialEditor)) {
            ImGui::End();
            return;
        }

        static constexpr std::pair<PipelineType, const char*> pipelines[] = {
            { PipelineType::LitStatic,   "LitStatic" },
            { PipelineType::UnlitStatic, "UnlitStatic" },
            { PipelineType::LitSkeletal, "LitSkeletal" },
        };
        const char* pipelinePreview = "LitStatic";
        for (const auto& [type, label] : pipelines) {
            if (type == materialDef.pipeline) pipelinePreview = label;
        }
        if (ImGui::BeginCombo("Pipeline", pipelinePreview)) {
            for (const auto& [type, label] : pipelines) {
                if (ImGui::Selectable(label, type == materialDef.pipeline)) materialDef.pipeline = type;
            }
            ImGui::EndCombo();
        }

        ImGui::SeparatorText("Textures");
        int removeTexture = -1;
        for (size_t slot = 0; slot < materialDef.textures.size(); ++slot) {
            ImGui::PushID(static_cast<int>(slot));
            MaterialTextureDef& texture = materialDef.textures[slot];

            ImGui::Text("Slot %zu", slot);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeTexture = static_cast<int>(slot);

            if (ImGui::RadioButton("Solid", texture.solid)) texture.solid = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("File", !texture.solid)) texture.solid = false;

            if (texture.solid) {
                ImGui::ColorEdit4("Color", &texture.color.x);
            } else {
                ImGui::InputText("Path", &texture.path);
                ImGui::Checkbox("sRGB", &texture.srgb);
            }

            std::string samplerName = nlohmann::json(texture.sampler).get<std::string>();
            if (ImGui::BeginCombo("Sampler", samplerName.c_str())) {
                for (int option = 0; option < static_cast<int>(SamplerType::Count); ++option) {
                    const auto candidate = static_cast<SamplerType>(option);
                    std::string candidateName = nlohmann::json(candidate).get<std::string>();
                    if (ImGui::Selectable(candidateName.c_str(), candidate == texture.sampler)) {
                        texture.sampler = candidate;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (removeTexture >= 0) materialDef.textures.erase(materialDef.textures.begin() + removeTexture);
        if (ImGui::Button("Add Texture")) materialDef.textures.push_back({});

        ImGui::SeparatorText("Uniform");
        ImGui::DragFloat2("UV Scale", &materialDef.uniform.uvScale.x, 0.01f);
        ImGui::DragFloat2("UV Offset", &materialDef.uniform.uvOffset.x, 0.01f);
        ImGui::DragFloat("Shininess", &materialDef.uniform.shininess, 1.0f, 1.0f, 512.0f);

        ImGui::SeparatorText("File");
        ImGui::InputText("Path", &materialPath);
        if (ImGui::Button("Save")) saveMaterialDef();
        ImGui::SameLine();
        if (ImGui::Button("Load")) loadMaterialDef();

        ImGui::End();
    }

    void EditorUI::saveMaterialDef() {
        nlohmann::json root;
        root["pipeline"] = materialDef.pipeline;

        nlohmann::json texturesJson = nlohmann::json::array();
        for (const MaterialTextureDef& texture : materialDef.textures) {
            nlohmann::json textureJson;
            textureJson["sampler"] = texture.sampler;
            if (texture.solid) {
                textureJson["solid"] = {
                    static_cast<int>(texture.color.r * 255.0f + 0.5f),
                    static_cast<int>(texture.color.g * 255.0f + 0.5f),
                    static_cast<int>(texture.color.b * 255.0f + 0.5f),
                    static_cast<int>(texture.color.a * 255.0f + 0.5f),
                };
            } else {
                textureJson["path"] = texture.path;
                textureJson["srgb"] = texture.srgb;
            }
            texturesJson.push_back(textureJson);
        }
        root["textures"] = texturesJson;
        root["uniform"] = materialDef.uniform;

        std::ofstream file(engine->getResourceManager()->resolveAssetPath(materialPath));
        if (!file.is_open()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not write material %s", materialPath.c_str());
            return;
        }
        file << root.dump(2);
        file.close();
        engine->getResourceManager()->reloadMaterial(materialPath);
        SDL_Log("Saved material %s", materialPath.c_str());
    }

    void EditorUI::loadMaterialDef() {
        std::ifstream file(engine->getResourceManager()->resolveAssetPath(materialPath));
        if (!file.is_open()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open material %s", materialPath.c_str());
            return;
        }
        nlohmann::json root;
        try {
            file >> root;
        } catch (const std::exception& error) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not parse material %s: %s", materialPath.c_str(), error.what());
            return;
        }

        materialDef = MaterialDef{};
        if (root.contains("pipeline")) root.at("pipeline").get_to(materialDef.pipeline);
        if (root.contains("textures")) {
            for (const auto& textureJson : root.at("textures")) {
                MaterialTextureDef texture;
                if (textureJson.contains("sampler")) textureJson.at("sampler").get_to(texture.sampler);
                if (textureJson.contains("solid")) {
                    texture.solid = true;
                    const auto& color = textureJson.at("solid");
                    if (color.size() >= 3) {
                        texture.color = glm::vec4(
                            color.at(0).get<int>() / 255.0f,
                            color.at(1).get<int>() / 255.0f,
                            color.at(2).get<int>() / 255.0f,
                            color.size() >= 4 ? color.at(3).get<int>() / 255.0f : 1.0f);
                    }
                } else if (textureJson.contains("path")) {
                    texture.solid = false;
                    texture.path = textureJson.at("path").get<std::string>();
                    texture.srgb = textureJson.value("srgb", false);
                }
                materialDef.textures.push_back(texture);
            }
        }
        if (root.contains("uniform")) root.at("uniform").get_to(materialDef.uniform);
    }

    void EditorUI::drawGizmo(){
        Entity* entity = engine->getEntity(selectedEntity);
        if (entity == nullptr) return;
        auto* transform = entity->getComponent<TransformComponent>();
        if (transform == nullptr) return;

        glm::mat4 view, projection;
        if (!engine->getCameraMatrices(view, projection)) return;

        auto* rigidbody = entity->getComponent<RigidbodyComponent>();
        const bool editingCollider = selectedCollider >= 0 && rigidbody != nullptr
                                     && selectedCollider < static_cast<int>(rigidbody->colliders.size());

        // Colliders carry no scale, so fall back to translate if scale is the active op.
        if (editingCollider && gizmoOperation == ImGuizmo::SCALE) gizmoOperation = ImGuizmo::TRANSLATE;

        float snapValue = snapTranslate;
        if (gizmoOperation == ImGuizmo::ROTATE) snapValue = snapRotateDegrees;
        else if (gizmoOperation == ImGuizmo::SCALE) snapValue = snapScale;
        const float snap[3] = { snapValue, snapValue, snapValue };

        if (editingCollider) {
            // Jolt places the body unscaled at the transform's position+rotation, so the collider
            // offset lives in that frame (entity scale is deliberately left out).
            const glm::mat4 bodyMatrix = glm::translate(glm::mat4(1.0f), transform->position)
                                       * glm::mat4_cast(transform->rotation);
            const physics::ColliderDef& c = rigidbody->colliders[selectedCollider];
            glm::mat4 model = bodyMatrix
                            * glm::translate(glm::mat4(1.0f), c.offset)
                            * glm::mat4_cast(c.rotation);

            if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                                     gizmoOperation, gizmoMode, glm::value_ptr(model),
                                     nullptr, gizmoSnap ? snap : nullptr)) {
                const glm::mat4 local = glm::inverse(bodyMatrix) * model;
                glm::vec3 scale, translation, skew;
                glm::quat rotation;
                glm::vec4 perspective;
                if (glm::decompose(local, scale, rotation, translation, skew, perspective)) {
                    rigidbody->setColliderTransform(selectedCollider, translation, rotation);
                }
            }
            return;
        }

        // Alt + drag the gizmo duplicates the selection (Unreal-style): on the frame the drag
        // begins with Alt held, clone the entity and switch the drag onto the clone. The gizmo
        // sits at the same pose, so ImGuizmo picks the clone up seamlessly.
        if (ImGui::GetIO().KeyAlt
            && selectedEntity != editor->getEditorCameraId()
            && ImGuizmo::IsOver()
            && !ImGuizmo::IsUsing()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (const Uint32 copy = duplicateEntity(*engine, selectedEntity)) {
                setSelected(copy);
                entity = engine->getEntity(copy);
                transform = entity ? entity->getComponent<TransformComponent>() : nullptr;
                if (!transform) return;
            }
        }

        glm::mat4 world = transform->worldMatrix();
        if (!ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                                  gizmoOperation, gizmoMode, glm::value_ptr(world),
                                  nullptr, gizmoSnap ? snap : nullptr)) {
            return;
        }

        // The gizmo edits the world pose; store it back relative to the parent.
        glm::mat4 local = world;
        if (Entity* parent = entity->getParent()) {
            if (auto* parentTransform = parent->getComponent<TransformComponent>()) {
                local = glm::inverse(parentTransform->worldMatrix()) * world;
            }
        }

        // use glm to decompose back into quaternion
        glm::vec3 scale, translation, skew;
        glm::quat rotation;
        glm::vec4 perspective;
        if (glm::decompose(local, scale, rotation, translation, skew, perspective)) {
            transform->position = translation;
            transform->rotation = rotation;
            transform->scale = scale;
        }
    }

    void EditorUI::drawColliderGizmoTarget(RigidbodyComponent* rigidbody){
        const int count = static_cast<int>(rigidbody->colliders.size());
        if (selectedCollider >= count) selectedCollider = -1;

        ImGui::SeparatorText("Gizmo Target");
        const std::string current = selectedCollider < 0
            ? std::string("Entity")
            : "Collider " + std::to_string(selectedCollider);
        if (ImGui::BeginCombo("##gizmoTarget", current.c_str())) {
            if (ImGui::Selectable("Entity", selectedCollider < 0)) selectedCollider = -1;
            for (int i = 0; i < count; ++i) {
                const std::string label = "Collider " + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), selectedCollider == i)) selectedCollider = i;
            }
            ImGui::EndCombo();
        }
    }

    std::vector<std::string> EditorUI::listSceneFiles() const {
        std::vector<std::string> names;
        const std::string dir = engine->getResourceManager()->resolveAssetPath("scenes");
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            // Only surface scene files, not stray assets that might live alongside them.
            if (name.size() > 11 && name.compare(name.size() - 11, 11, ".scene.json") == 0) {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    void EditorUI::drawMenuBar(){
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::BeginMenu("Open")) {
                const std::vector<std::string> scenes = listSceneFiles();
                if (scenes.empty()) ImGui::TextDisabled("(no scenes)");
                for (const std::string& name : scenes) {
                    const std::string path = "scenes/" + name;
                    if (ImGui::MenuItem(name.c_str(), nullptr, path == editor->getCurrentScenePath())) {
                        editor->openScene(path);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Reload")) editor->openScene(editor->getCurrentScenePath());
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) editor->saveCurrentScene();
            if (ImGui::MenuItem("Save As...")) {
                // Seed the dialog with the current filename (path minus the "scenes/" prefix).
                const std::string& current = editor->getCurrentScenePath();
                saveAsName = current.substr(current.find_last_of('/') + 1);
                openSaveAsRequested = true;
            }
            ImGui::EndMenu();
        }

        // Right-aligned: which scene is open and how long ago it was saved.
        const std::string& scenePath = editor->getCurrentScenePath();
        const std::string sceneName = scenePath.substr(scenePath.find_last_of('/') + 1);
        std::string status = sceneName + "  -  ";
        const Uint64 savedTick = editor->getLastSaveTick();
        if (savedTick == 0) {
            status += "not saved yet";
        } else {
            const Uint64 secs = (SDL_GetTicks() - savedTick) / 1000;
            if (secs < 60) status += "saved " + std::to_string(secs) + "s ago";
            else status += "saved " + std::to_string(secs / 60) + "m ago";
        }
        const float textWidth = ImGui::CalcTextSize(status.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - textWidth - 12.0f);
        ImGui::TextUnformatted(status.c_str());

        ImGui::EndMainMenuBar();
    }

    void EditorUI::drawSaveAsDialog(){
        if (openSaveAsRequested) {
            ImGui::OpenPopup("Save Scene As");
            openSaveAsRequested = false;
        }

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

        ImGui::TextUnformatted("assets/scenes/");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetNextItemWidth(220.0f);
        const bool submit = ImGui::InputText("##saveasname", &saveAsName,
                                             ImGuiInputTextFlags_EnterReturnsTrue);

        const bool valid = !saveAsName.empty();
        ImGui::BeginDisabled(!valid);
        if ((ImGui::Button("Save") || submit) && valid) {
            std::string name = saveAsName;
            // Confine saves to the scenes dir and normalize the extension.
            if (name.find(".json") == std::string::npos) name += ".scene.json";
            editor->saveSceneAs("scenes/" + name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void EditorUI::drawToolbar(){
        // Toolbar: play/stop the simulation, then the gizmo controls
        ImGui::Begin("Toolbar");
        const bool simulating = engine->isSimulating();
        if (ImGui::Button(simulating ? "Stop" : "Play")) {
            if (simulating) editor->stop();
            else editor->play();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(simulating ? "Simulating" : "Paused");

        ImGui::SameLine(0.0f, 20.0f);
        if (ImGui::RadioButton("Translate", gizmoOperation == ImGuizmo::TRANSLATE)) gizmoOperation = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", gizmoOperation == ImGuizmo::ROTATE)) gizmoOperation = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale", gizmoOperation == ImGuizmo::SCALE)) gizmoOperation = ImGuizmo::SCALE;

        // ImGuizmo always scales in local space, so the toggle would be a lie there.
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::BeginDisabled(gizmoOperation == ImGuizmo::SCALE);
        bool worldSpace = gizmoMode == ImGuizmo::WORLD;
        if (ImGui::Checkbox("World", &worldSpace)) {
            gizmoMode = worldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 20.0f);
        ImGui::Checkbox("Snap", &gizmoSnap);
        if (gizmoSnap) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (gizmoOperation == ImGuizmo::ROTATE) {
                ImGui::DragFloat("##snap", &snapRotateDegrees, 1.0f, 1.0f, 90.0f, "%.0f deg");
            } else if (gizmoOperation == ImGuizmo::SCALE) {
                ImGui::DragFloat("##snap", &snapScale, 0.01f, 0.01f, 10.0f, "%.2f");
            } else {
                ImGui::DragFloat("##snap", &snapTranslate, 0.1f, 0.01f, 100.0f, "%.2f");
            }
        }
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::Checkbox("Show Physics Shapes", &engine->showPhysicsShapes);

        // Grid dropdown: toggle the world-unit grid and adjust its cell size / extent.
        ImGui::SameLine(0.0f, 20.0f);
        if (ImGui::Button("Grid")) ImGui::OpenPopup("GridMenu");
        if (ImGui::BeginPopup("GridMenu")) {
            ImGui::Checkbox("Show Grid", &engine->showGrid);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Cell Size", &engine->gridSpacing, 0.1f, 0.01f, 1000.0f, "%.2f");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragInt("Extent", &engine->gridExtent, 1.0f, 1, 200);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Opacity", &engine->gridOpacity, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::EndPopup();
        }

        ImGui::SameLine(0.0f, 20.0f);
        if (ImGui::Button("Material Editor")) showMaterialEditor = !showMaterialEditor;
        ImGui::End();
    }

    void EditorUI::drawOutliner(){
        // Outliner: root entities as a tree, children nested under their parent. Click to select.
        ImGui::Begin("Outliner");
        if (ImGui::Button("Add Entity")) {
            Entity* created = engine->addEntity();
            created->addComponent<TransformComponent>();
            setSelected(created->getId());
        }
        std::vector<Uint32> rootIds;
        for (const auto& [id, entity] : engine->getEntities()) {
            if (entity && entity->getParent() == nullptr) rootIds.push_back(id);
        }
        std::sort(rootIds.begin(), rootIds.end());
        for (const Uint32 id : rootIds) drawOutlinerNode(engine->getEntity(id));
        ImGui::End();
    }

    void EditorUI::drawInspector(){
        // Inspector: name + each component's own widgets
        ImGui::Begin("Inspector");
        bool deleteEntityRequested = false;
        if (Entity* entity = engine->getEntity(selectedEntity)) {
            std::string name = entity->getName();
            if (ImGui::InputText("Name", &name)) {
                entity->setName(name);
            }
            ImGui::Text("Entity Id: %u", selectedEntity);
            drawParentSelector(entity);

            if (selectedEntity != editor->getEditorCameraId() && ImGui::Button("Delete Entity")) {
                deleteEntityRequested = true;
            }
            ImGui::Separator();

            Component* componentToRemove = nullptr;
            for (const auto& component : entity->getComponents()) {
                ImGui::PushID(component.get());
                if (ImGui::CollapsingHeader(component->getTypeName(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    component->drawInspector();
                    if (auto* render = dynamic_cast<RenderComponent*>(component.get())) {
                        drawRenderComponentAssets(render);
                    }
                    if (auto* rigidbody = dynamic_cast<RigidbodyComponent*>(component.get())) {
                        drawColliderGizmoTarget(rigidbody);
                    }
                    if (ImGui::SmallButton("Remove Component")) componentToRemove = component.get();
                }
                ImGui::PopID();
            }
            if (componentToRemove != nullptr) entity->removeComponent(componentToRemove);

            ImGui::Separator();
            if (ImGui::Button("Add Component")) ImGui::OpenPopup("AddComponentPopup");
            if (ImGui::BeginPopup("AddComponentPopup")) {
                for (const ComponentTypeInfo& info : engine->getComponentRegistry().getTypes()) {
                    bool alreadyHas = false;
                    for (const auto& existing : entity->getComponents()) {
                        if (info.id == existing->serialId()) { alreadyHas = true; break; }
                    }
                    if (alreadyHas) continue;

                    if (ImGui::MenuItem(info.displayName.c_str())) {
                        if (auto component = engine->getComponentRegistry().create(info.id)) {
                            // A fresh RenderComponent defaults to the sphere mesh + material.
                            if (auto* render = dynamic_cast<RenderComponent*>(component.get())) {
                                ResourceManager* resources = engine->getResourceManager();
                                render->setMesh(resources->getMesh("models/sphere.glb"));
                                render->addMaterial(resources->getMaterial("materials/sphere.mat"));
                            }
                            entity->addComponent(std::move(component));
                        }
                    }
                }
                ImGui::EndPopup();
            }
        } else {
            ImGui::TextDisabled("No entity selected");
        }
        ImGui::End();

        if (deleteEntityRequested) {
            const Uint32 toDelete = selectedEntity;
            setSelected(0);
            engine->removeEntity(toDelete);
        }
    }

    void EditorUI::draw(){
        drawMenuBar();

        ImGuizmo::BeginFrame();
        ImGuizmo::SetOrthographic(false);
        // panels just draw on top of the swapchain
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGuizmo::SetRect(viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y);

        // Full-window dockspace with a passthrough center, so the 3D scene shows through the
        // middle and panels dock around it. Build a default layout the first time only
        const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            ImGui::DockBuilderAddNode(dockspaceId,
                ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

            ImGuiID center = dockspaceId;
            const ImGuiID topId   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Up,    0.06f, nullptr, &center);
            const ImGuiID leftId  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.18f, nullptr, &center);
            const ImGuiID rightId = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);

            ImGui::DockBuilderDockWindow("Toolbar", topId);
            ImGui::DockBuilderDockWindow("Outliner", leftId);
            ImGui::DockBuilderDockWindow("Inspector", rightId);
            ImGui::DockBuilderFinish(dockspaceId);
        }
        ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(),
            ImGuiDockNodeFlags_PassthruCentralNode);

        drawToolbar();
        drawOutliner();
        drawInspector();

        drawMaterialEditor();
        drawSaveAsDialog();
        drawGizmo();
    }
} // ytail
