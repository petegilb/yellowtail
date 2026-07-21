//
// Created by PeterPC on 7/14/2026.
//

#include "Editor.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/Engine.h"
#include "engine/Entity.h"
#include "engine/components/TransformComponent.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/FreeMovementComponent.h"
#include "engine/serialize/SceneSerializer.h"

#include <nlohmann/json.hpp>

namespace ytail
{
    // Engine null-check + assert live in the Application base constructor.
    Editor::Editor(Engine* inEngine) : Application(inEngine), ui(this) {
    }

    Editor::~Editor(){
        SDL_Log("Editor destroyed!");
    }

    void Editor::start(){
        SDL_Log("Editor started!");

        engine->setPlayState(PlayState::Paused);
        engine->showPhysicsShapes = true;
        engine->showGrid = true;
        engine->showLightGizmos = true;
        engine->showEditorIcons = true;

        loadScene(*engine, currentScenePath);
        createEditorCamera();
    }

    void Editor::createEditorCamera(){
        Entity* camera = engine->addEntity();
        camera->setName("FlyCam");
        // editor-only, never written into a scene
        camera->setSerializable(false);

        const auto camTransform = camera->addComponent<TransformComponent>();
        camera->addComponent<CameraComponent>();
        camera->addComponent<FreeMovementComponent>();
        camTransform->position = glm::vec3(0.0f, 3.0f, 5.0f);   // back up, looking down -Z toward origin
        camTransform->setRotationEuler(glm::vec3(-30.0f, 0.0f, 0.0f));

        editorCameraId = camera->getId();
        engine->setActiveCamera(editorCameraId);
    }

    bool Editor::captureCameraPose(glm::vec3& outPosition, glm::quat& outRotation) const {
        Entity* camera = engine->getEntity(editorCameraId);
        if (!camera) return false;
        auto* transform = camera->getComponent<TransformComponent>();
        if (!transform) return false;
        outPosition = transform->position;
        outRotation = transform->rotation;
        return true;
    }

    void Editor::applyCameraPose(const glm::vec3& position, const glm::quat& rotation){
        Entity* camera = engine->getEntity(editorCameraId);
        if (!camera) return;
        auto* transform = camera->getComponent<TransformComponent>();
        if (!transform) return;
        transform->position = position;
        transform->rotation = rotation;
    }

    void Editor::openScene(const std::string& path){
        // Preserve the fly-cam pose so swapping/reloading scenes doesn't jump the view.
        glm::vec3 camPos(0.0f);
        glm::quat camRot(1.0f, 0.0f, 0.0f, 0.0f);
        const bool hadPose = captureCameraPose(camPos, camRot);

        if (!loadScene(*engine, path)) return;
        currentScenePath = path;
        createEditorCamera();
        if (hadPose) applyCameraPose(camPos, camRot);
        ui.setSelected(0);
    }

    void Editor::saveCurrentScene(){
        if (saveScene(*engine, currentScenePath)) lastSaveTick = SDL_GetTicks();
    }

    bool Editor::saveSceneAs(const std::string& path){
        if (!saveScene(*engine, path)) return false;
        currentScenePath = path;
        lastSaveTick = SDL_GetTicks();
        return true;
    }

    void Editor::play(){
        // Play: snapshot the live scene (including unsaved edits), then simulate.
        playSnapshot = std::make_unique<nlohmann::json>(saveSceneToJson(*engine));
        engine->setPlayState(PlayState::Simulating);
    }

    void Editor::stop(){
        // Stop: pause and restore the scene to its pre-Play state, keeping the fly-cam pose.
        engine->setPlayState(PlayState::Paused);
        if (playSnapshot) {
            glm::vec3 camPos(0.0f);
            glm::quat camRot(1.0f, 0.0f, 0.0f, 0.0f);
            const bool hadPose = captureCameraPose(camPos, camRot);
            loadSceneFromJson(*engine, *playSnapshot);
            createEditorCamera();
            if (hadPose) applyCameraPose(camPos, camRot);
        }
        ui.setSelected(0);
    }

    void Editor::eventTick(const SDL_Event& event){
        if (event.type == SDL_EVENT_KEY_DOWN){
            handleInput(event.key);
            return;
        }
        if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) return;

        ui.handleSceneClick(event.button.x, event.button.y);
    }

    void Editor::tick(float deltaTime){
    }

    void Editor::uiTick(){
        ui.draw();
    }

    void Editor::handleInput(const SDL_KeyboardEvent& keyboard_event){
        if (keyboard_event.key == SDLK_ESCAPE) {
            engine->quit();
            return;
        }

        // Ctrl+S saves the current scene, even while flying the camera.
        if (keyboard_event.key == SDLK_S && (keyboard_event.mod & SDL_KMOD_CTRL)) {
            saveCurrentScene();
            return;
        }

        ui.handleKey(keyboard_event);
    }
} // ytail
