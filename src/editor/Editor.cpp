//
// Created by PeterPC on 7/14/2026.
//

#include "Editor.h"

#include "engine/Engine.h"
#include "engine/components/RenderComponent.h"
#include "engine/components/TransformComponent.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/FreeMovementComponent.h"
#include "engine/components/LightComponent.h"
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
        cube->addComponent<TransformComponent>();
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
    }

    void Editor::eventTick(const SDL_Event& event){
        if (event.type == SDL_EVENT_KEY_DOWN){
            handleInput(event.key);
        }
    }

    void Editor::tick(float deltaTime){
    }

    void Editor::handleInput(const SDL_KeyboardEvent& keyboard_event){
        if (keyboard_event.key == SDLK_ESCAPE) {
            engine->quit();
        }
    }
} // ytail
