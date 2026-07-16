//
// Created by PeterPC on 7/14/2026.
//

#include "Editor.h"

#include <algorithm>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include "engine/Engine.h"
#include "engine/Entity.h"
#include "engine/Component.h"
#include "engine/components/RenderComponent.h"
#include "engine/components/TransformComponent.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/FreeMovementComponent.h"
#include "engine/components/LightComponent.h"
#include "engine/components/RigidbodyComponent.h"
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

        // Editor opens in edit mode; the user presses Play to simulate.
        engine->setSimulating(false);
        // TODO make the scene reset when turning off simulating (it should load from our scene file once that exists)

        ResourceManager* resourceManager = engine->getResourceManager();

        // scene setup
        Entity* camera = engine->addEntity();
        const auto camTransform = camera->addComponent<TransformComponent>();
        camera->addComponent<CameraComponent>();
        engine->setActiveCamera(camera->getId());
        camTransform->position = glm::vec3(0.0f, 3.0f, 5.0f);   // back up 5 units, looking down -Z toward origin
        camTransform->setRotationEuler(glm::vec3(-30.0f, 0.0f, 0.0f));
        camera->addComponent<FreeMovementComponent>();

        Entity* light0 = engine->addEntity();
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
        cube2Render->outline = true;
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

        // physics: static floor collider, sitting so its top lines up with the visual floor (y = -2)
        Entity* floorBody = engine->addEntity();
        auto floorBodyTransform = floorBody->addComponent<TransformComponent>();
        floorBodyTransform->position = glm::vec3(0.0f, -3.0f, 0.0f);
        auto floorCollider = floorBody->addComponent<RigidbodyComponent>();
        floorCollider->type = physics::BodyType::Static;
        floorCollider->shape = physics::ColliderShape::Box;
        floorCollider->halfExtents = glm::vec3(50.0f, 1.0f, 50.0f);

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
        }
    }

    void Editor::tick(float deltaTime){
    }

    void Editor::onImGui(){
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

        // Toolbar: play/pause the fixed-step simulation
        ImGui::Begin("Toolbar");
        const bool simulating = engine->isSimulating();
        if (ImGui::Button(simulating ? "Pause" : "Play")) {
            engine->setSimulating(!simulating);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(simulating ? "Simulating" : "Paused");
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
                selectedEntity = id;
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
    }

    void Editor::handleInput(const SDL_KeyboardEvent& keyboard_event){
        if (keyboard_event.key == SDLK_ESCAPE) {
            engine->quit();
        }
    }
} // ytail
