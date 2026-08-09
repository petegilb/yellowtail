//
// Created by Peter Gilbert on 8/8/26.
//

#include "NetworkComponent.h"

#include "imgui.h"

namespace ytail {
    void NetworkComponent::drawInspector() {
        const char* authorityNames[] = { "Host", "Owner", "Remote" };
        ImGui::Text("Net Id: %u", netId);
        if (ownerPeerId == 0) {
            ImGui::TextUnformatted("Owner: host");
        } else {
            ImGui::Text("Owner: peer %u", ownerPeerId);
        }
        ImGui::Text("Authority: %s", authorityNames[static_cast<int>(authority)]);
    }
} // ytail
