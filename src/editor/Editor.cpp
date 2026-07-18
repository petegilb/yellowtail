//
// Created by PeterPC on 7/14/2026.
//

#include "Editor.h"

#include <algorithm>
#include <cfloat>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
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
#include "engine/components/CameraComponent.h"
#include "engine/components/FreeMovementComponent.h"
#include "engine/components/LightComponent.h"
#include "engine/components/RigidbodyComponent.h"
#include "engine/render/Mesh.h"
#include "engine/managers/ResourceManager.h"

namespace ytail
{
    // Engine null-check + assert live in the Application base constructor.
    Editor::Editor(Engine* inEngine) : Application(inEngine) {
    }

    Editor::~Editor(){
        SDL_Log("Editor destroyed!");
    }

    void Editor::start(){
        SDL_Log("Editor started!");

        engine->setPlayState(PlayState::Paused);
        // TODO make the scene reset when turning off simulating (it should load from our scene file once that exists)

        ResourceManager* resourceManager = engine->getResourceManager();

        // scene setup
        Entity* camera = engine->addEntity();
        camera->setName("FlyCam");
        const auto camTransform = camera->addComponent<TransformComponent>();
        camera->addComponent<CameraComponent>();
        engine->setActiveCamera(camera->getId());
        camTransform->position = glm::vec3(0.0f, 3.0f, 5.0f);   // back up 5 units, looking down -Z toward origin
        camTransform->setRotationEuler(glm::vec3(-30.0f, 0.0f, 0.0f));
        camera->addComponent<FreeMovementComponent>();

        Entity* light0 = engine->addEntity();
        light0->setName("Light1");
        const auto lightTransform = light0->addComponent<TransformComponent>();
        const auto lightComponent = light0->addComponent<LightComponent>();
        lightComponent->color = glm::vec3(1.0f, 1.0f, 1.0f);
        lightComponent->intensity = 1.0f;
        lightTransform->position = glm::vec3(1.2f, 1.0f, 2.0f);  // camera side, up and to the right

        // create material
        auto material = std::make_shared<Material>();
        material->pipelineType = PipelineType::LitStatic;
        // diffuse (color -> sRGB) at t0, specular (data mask -> linear) at t1, in slot order.
        SDL_GPUSampler* sampler = resourceManager->getSampler(SamplerType::LinearWrap);
        material->textures.push_back({ resourceManager->getTexture("textures/container2.png", true), sampler });
        material->textures.push_back({ resourceManager->getTexture("textures/container2_specular.png", false), sampler });
        // material uniform (b1 space3): just shininess for now.
        // https://learnopengl.com/Advanced-Lighting/Advanced-Lighting
        // hardcoded exponent value to 64
        MaterialUniform matUniform{};
        matUniform.shininess = 64.0f;
        material->setUniform(matUniform);

        Entity* cube = engine->addEntity();
        auto cubeTransform = cube->addComponent<TransformComponent>();
        cubeTransform->scale = glm::vec3(0.5);
        auto cubeRender = cube->addComponent<RenderComponent>();
        // add mesh and materials to render component
        std::shared_ptr<Mesh> cubeMesh = resourceManager->getMesh("models/cube.glb");
        cubeRender->setMesh(cubeMesh);
        cubeRender->addMaterial(material);

        Entity* cube2 = engine->addEntity();
        auto cube2Transform = cube2->addComponent<TransformComponent>();
        auto cube2Render = cube2->addComponent<RenderComponent>();
        cube2Render->setMesh(cubeMesh);
        cube2Render->addMaterial(material);
        cube2Transform->position = glm::vec3(2.0f, -1.0f, -5.0f);;

        // create floor
        auto floorMaterial = std::make_shared<Material>();
        floorMaterial->pipelineType = PipelineType::LitStatic;
        floorMaterial->textures.push_back({ resourceManager->getTexture("textures/wood.png", true), sampler });
        floorMaterial->textures.push_back({ resourceManager->getSolidTexture(255), sampler });
        MaterialUniform floorMatUniform{};
        floorMatUniform.shininess = 64.0f;
        floorMatUniform.uvScale = glm::vec2(4.00f);
        floorMaterial->setUniform(floorMatUniform);

        Entity* floor = engine->addEntity();
        auto floorTransform = floor->addComponent<TransformComponent>();
        floorTransform->position = glm::vec3(0.0f, -2.0f, 0.0f);
        auto floorRender = floor->addComponent<RenderComponent>();
        std::shared_ptr<Mesh> floorMesh = resourceManager->getMesh("models/floor.glb");
        floorRender->setMesh(floorMesh);
        floorRender->addMaterial(floorMaterial);
        auto floorCollider = floor->addComponent<RigidbodyComponent>();
        floorCollider->type = physics::BodyType::Static;
        floorCollider->shape = physics::ColliderShape::Box;
        floorCollider->halfExtents = glm::vec3(10.0f, 0.1f, 10.0f);

        // physics: a sphere that falls onto the floor
        Entity* sphere = engine->addEntity();
        auto sphereTransform = sphere->addComponent<TransformComponent>();
        sphereTransform->position = glm::vec3(0.0f, 5.0f, 0.0f);
        auto sphereBody = sphere->addComponent<RigidbodyComponent>();
        sphereBody->type = physics::BodyType::Dynamic;
        sphereBody->shape = physics::ColliderShape::Sphere;
        sphereBody->radius = 1.0f;
        
        // sphere material
        auto sphereMaterial = std::make_shared<Material>();
        sphereMaterial->pipelineType = PipelineType::LitStatic;
        sphereMaterial->textures.push_back({ resourceManager->getSolidTexture(187, 108, 224), sampler });
        sphereMaterial->textures.push_back({ resourceManager->getSolidTexture(255), sampler });
        auto sphereRender = sphere->addComponent<RenderComponent>();
        std::shared_ptr<Mesh> sphereMesh = resourceManager->getMesh("models/sphere.glb");
        sphereRender->setMesh(sphereMesh);
        sphereRender->addMaterial(sphereMaterial);
    }

    void Editor::eventTick(const SDL_Event& event){
        if (event.type == SDL_EVENT_KEY_DOWN){
            handleInput(event.key);
            return;
        }
        if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) return;

        // Only trust the gizmo hit-test while something is selected
        const bool overGizmo = selectedEntity != 0 && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
        if (ImGui::GetIO().WantCaptureMouse
            || Input::get().isMouseCaptured()
            || overGizmo) {
            return;
        }

        selectAtScreen(event.button.x, event.button.y);
    }

    void Editor::tick(float deltaTime){
    }

    bool rayAabb(const glm::vec3& origin, const glm::vec3& dir,
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

    void Editor::selectAtScreen(float screenX, float screenY){
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
            const glm::mat4 invModel = glm::inverse(transform->modelMatrix());
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

    void Editor::setSelected(Uint32 id){
        if (id == selectedEntity) return;

        if (Entity* previous = engine->getEntity(selectedEntity)){
            if (auto* renderComp = previous->getComponent<RenderComponent>()){
                renderComp->outline = false;
            }
        }

        selectedEntity = id;

        if (Entity* current = engine->getEntity(selectedEntity)){
            if (auto* renderComp = current->getComponent<RenderComponent>()){
                renderComp->outline = true;
            }
        }
    }

    void Editor::drawGizmo(){
        Entity* entity = engine->getEntity(selectedEntity);
        if (entity == nullptr) return;
        auto* transform = entity->getComponent<TransformComponent>();
        if (transform == nullptr) return;

        glm::mat4 view, projection;
        if (!engine->getCameraMatrices(view, projection)) return;

        float snapValue = snapTranslate;
        if (gizmoOperation == ImGuizmo::ROTATE) snapValue = snapRotateDegrees;
        else if (gizmoOperation == ImGuizmo::SCALE) snapValue = snapScale;
        const float snap[3] = { snapValue, snapValue, snapValue };

        glm::mat4 model = transform->modelMatrix();
        if (!ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                                  gizmoOperation, gizmoMode, glm::value_ptr(model),
                                  nullptr, gizmoSnap ? snap : nullptr)) {
            return;
        }

        // use glm to decompose back into quaternion
        glm::vec3 scale, translation, skew;
        glm::quat rotation;
        glm::vec4 perspective;
        if (glm::decompose(model, scale, rotation, translation, skew, perspective)) {
            transform->position = translation;
            transform->rotation = rotation;
            transform->scale = scale;
        }
    }

    void Editor::uiTick(){
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

        // Toolbar: play/pause the fixed-step simulation, then the gizmo controls
        ImGui::Begin("Toolbar");
        const bool simulating = engine->isSimulating();
        if (ImGui::Button(simulating ? "Pause" : "Play")) {
            simulating ? engine->setPlayState(PlayState::Paused) : engine->setPlayState(PlayState::Simulating);
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
        ImGui::End();

        // Outliner: every entity, sorted by id for a stable order, click to select
        ImGui::Begin("Outliner");
        std::vector<Uint32> ids;
        ids.reserve(engine->getEntities().size());
        for (const auto& [id, entity] : engine->getEntities()) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        for (const Uint32 id : ids) {
            Entity* entity = engine->getEntity(id);
            if (!entity) continue;
            if (ImGui::Selectable(entity->getName().c_str(), id == selectedEntity)) {
                setSelected(id);
            }
        }
        ImGui::End();

        // Inspector: name + each component's own widgets
        ImGui::Begin("Inspector");
        if (Entity* entity = engine->getEntity(selectedEntity)) {
            char nameBuffer[128];
            SDL_strlcpy(nameBuffer, entity->getName().c_str(), sizeof(nameBuffer));
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
                entity->setName(nameBuffer);
            }
            ImGui::Text("Entity Id: %u", selectedEntity);
            ImGui::Separator();
            for (const auto& component : entity->getComponents()) {
                ImGui::PushID(component.get());
                if (ImGui::CollapsingHeader(component->getTypeName(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    component->drawInspector();
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextDisabled("No entity selected");
        }
        ImGui::End();

        drawGizmo();
    }

    void Editor::handleInput(const SDL_KeyboardEvent& keyboard_event){
        if (keyboard_event.key == SDLK_ESCAPE) {
            engine->quit();
            return;
        }

        // Gizmo modes. disabled when flying
        if (Input::get().isMouseCaptured() || ImGui::GetIO().WantTextInput) return;
        switch (keyboard_event.key) {
            case SDLK_W: gizmoOperation = ImGuizmo::TRANSLATE; break;
            case SDLK_E: gizmoOperation = ImGuizmo::ROTATE; break;
            case SDLK_R: gizmoOperation = ImGuizmo::SCALE; break;
            default: ;
        }
    }
} // ytail
