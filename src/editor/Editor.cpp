//
// Created by PeterPC on 7/14/2026.
//

#include "Editor.h"

#include <fstream>

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
    // Also write the scene into the project's source assets tree (when that path was baked in at
    // build time), so editor saves land in the git repo, not only the exe-relative build copy.
    static void mirrorSceneToSource(Engine& engine, const std::string& path) {
#ifdef YT_EDITOR_SOURCE_ASSETS_DIR
        const std::string full = std::string(YT_EDITOR_SOURCE_ASSETS_DIR) + path;
        std::ofstream file(full);
        if (file.is_open()) {
            file << saveSceneToJson(engine).dump(2);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not write scene to source tree: %s", full.c_str());
        }
#else
        (void)engine;
        (void)path;
#endif
    }

    // Engine null-check + assert live in the Application base constructor.
    Editor::Editor(Engine* inEngine) : Application(inEngine), ui(this) {
    }

    Editor::~Editor(){
        // Kill the local-test instances we spawned so closing the editor closes them too.
        for (SDL_Process* process : spawnedProcesses) {
            SDL_KillProcess(process, true);
            SDL_DestroyProcess(process);
        }
        SDL_Log("Editor destroyed!");
    }

    void Editor::launchLocalMultiplayer(int instanceCount) {
        if (gameExecutable.empty() || instanceCount < 1) return;

        for (int i = 0; i < instanceCount; ++i) {
            const std::string indexArg = std::to_string(i);
            // Instance 0 hosts on the local port; the rest connect to it.
            const char* const args[] = {
                gameExecutable.c_str(),
                "local",
                i == 0 ? "host" : "connect",
                "--window-index", indexArg.c_str(),
                nullptr
            };
            SDL_Process* process = SDL_CreateProcess(args, false);
            if (process == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch %s: %s",
                             gameExecutable.c_str(), SDL_GetError());
                continue;
            }
            spawnedProcesses.push_back(process);
        }
    }

    void Editor::start(){
        SDL_Log("Editor started!");

        engine->setPlayState(PlayState::Paused);
        engine->showPhysicsShapes = true;
        engine->showGrid = true;
        engine->showLightGizmos = true;
        engine->showEditorIcons = true;
        engine->showShadows = true;

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
        camTransform->setPosition(glm::vec3(0.0f, 3.0f, 5.0f)); // back up, looking down -Z toward origin
        camTransform->setRotationEuler(glm::vec3(-30.0f, 0.0f, 0.0f));

        editorCameraId = camera->getId();
        engine->setActiveCamera(editorCameraId);
        // The editor always drives its own fly cam, never the scene's cameras.
        engine->possess(editorCameraId);
    }

    bool Editor::captureCameraPose(glm::vec3& outPosition, glm::quat& outRotation) const {
        Entity* camera = engine->getEntity(editorCameraId);
        if (!camera) return false;
        auto* transform = camera->getComponent<TransformComponent>();
        if (!transform) return false;
        outPosition = transform->getPosition();
        outRotation = transform->getRotation();
        return true;
    }

    void Editor::applyCameraPose(const glm::vec3& position, const glm::quat& rotation){
        Entity* camera = engine->getEntity(editorCameraId);
        if (!camera) return;
        auto* transform = camera->getComponent<TransformComponent>();
        if (!transform) return;
        transform->setPosition(position);
        transform->setRotation(rotation);
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
        if (saveScene(*engine, currentScenePath)) {
            mirrorSceneToSource(*engine, currentScenePath);
            lastSaveTick = SDL_GetTicks();
        }
    }

    bool Editor::saveSceneAs(const std::string& path){
        if (!saveScene(*engine, path)) return false;
        mirrorSceneToSource(*engine, path);
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
