//
// Created by Peter Gilbert on 8/8/26.
//

#include "NetworkComponent.h"

#include "imgui.h"

namespace ytail {
    void NetworkComponent::drawInspector() {
        ImGui::Text("Net Id: %u", netId);
        if (ownerPeerId == 0) {
            ImGui::TextUnformatted("Owner: nobody");
        } else {
            ImGui::Text("Owner: peer %u", ownerPeerId);
        }
    }
} // ytail
