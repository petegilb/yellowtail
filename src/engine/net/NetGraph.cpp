//
// Created by Peter Gilbert on 8/14/26.
//

#include "NetGraph.h"

#include <algorithm>

#include <SDL3/SDL_stdinc.h>

#include "imgui.h"

namespace ytail::net {
    namespace {
        constexpr float ColumnWidth = 2.0f;
        constexpr float MainLaneHeight = 46.0f;
        constexpr float ResimLaneHeight = 14.0f;
        constexpr float EventLaneHeight = 5.0f;
        constexpr float LaneGap = 2.0f;
        constexpr float GraphWidth = static_cast<float>(NetGraph::Length) * ColumnWidth;
        // A full height resim bar. Past this the client snaps instead of replaying anyway.
        constexpr float ResimScaleTicks = 24.0f;
        // Floors under the auto scales, so an idle connection does not draw its noise full height.
        constexpr float MinCorrectionScale = 0.05f;
        constexpr float MinByteScale = 128.0f;
        // Ticks per grid line: one second at the fixed rate.
        constexpr int GridSpacing = 60;

        constexpr ImU32 LaneBackground = IM_COL32(10, 12, 16, 170);
        constexpr ImU32 GridLine = IM_COL32(255, 255, 255, 22);
        constexpr ImU32 ToleranceLine = IM_COL32(120, 200, 255, 70);
        constexpr ImU32 ScaleLabel = IM_COL32(255, 255, 255, 110);
        constexpr ImU32 KeptColor = IM_COL32(90, 200, 110, 255);
        constexpr ImU32 ReplayColor = IM_COL32(240, 185, 70, 255);
        constexpr ImU32 SnapColor = IM_COL32(235, 75, 60, 255);
        constexpr ImU32 ResimColor = IM_COL32(150, 145, 245, 255);
        constexpr ImU32 SentColor = IM_COL32(90, 190, 210, 255);
        constexpr ImU32 ReceivedColor = IM_COL32(120, 150, 235, 255);
        constexpr ImU32 LostColor = IM_COL32(235, 75, 60, 255);
        constexpr ImU32 StarvedColor = IM_COL32(245, 150, 50, 255);
        constexpr ImU32 AppliedColor = IM_COL32(70, 115, 160, 255);

        void drawBar(ImDrawList* draw, const float x, const float bottom, const float laneHeight,
                     const float fraction, const ImU32 color) {
            // Anything that happened at all gets two pixels: its colour is most of the report, and a
            // one-pixel event is invisible next to a lane full of tall bars.
            const float height = std::max(std::min(fraction, 1.0f) * laneHeight, 2.0f);
            draw->AddRectFilled(ImVec2(x, bottom - height), ImVec2(x + ColumnWidth, bottom), color);
        }

        void legendEntry(const char* label, const ImU32 color, const bool first = false) {
            if (!first) {
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextDisabled("/");
                ImGui::SameLine(0.0f, 4.0f);
            }
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%s", label);
        }
    }

    void NetGraph::push(const NetGraphSample& sample) {
        samples[next] = sample;
        next = (next + 1) % Length;
    }

    const NetGraphSample& NetGraph::at(const size_t column) const {
        return samples[(next + column) % Length];
    }

    void NetGraph::clear() {
        samples.fill({});
        next = 0;
    }

    void NetGraph::updateScales() {
        float peakCorrection = 0.0f;
        float peakBytes = 0.0f;
        for (const NetGraphSample& sample : samples) {
            peakCorrection = std::max(peakCorrection, sample.correction);
            peakBytes = std::max(peakBytes,
                                 static_cast<float>(std::max(sample.bytesSent, sample.bytesReceived)));
        }

        // Up immediately, so a spike is drawn at the height it actually happened; down slowly, so the
        // bars either side of it stay comparable after it falls off the end.
        const float correctionTarget = std::max(peakCorrection, MinCorrectionScale);
        correctionScale = correctionTarget > correctionScale
                        ? correctionTarget
                        : correctionScale + (correctionTarget - correctionScale) * 0.05f;
        const float byteTarget = std::max(peakBytes, MinByteScale);
        byteScale = byteTarget > byteScale ? byteTarget : byteScale + (byteTarget - byteScale) * 0.05f;
    }

    void NetGraph::draw(const NetGraphStats& stats) {
        if (!visible) return;
        updateScales();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        constexpr float padding = 12.0f;
        const bool right = (corner & 1) != 0;
        const bool bottom = (corner & 2) != 0;
        const ImVec2 pivot(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f);
        const ImVec2 position(viewport->WorkPos.x + (right ? viewport->WorkSize.x - padding : padding),
                              viewport->WorkPos.y + (bottom ? viewport->WorkSize.y - padding : padding));
        ImGui::SetNextWindowPos(position, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(0.35f);
        // NoInputs is the point of the window: it draws over the game and the mouse goes straight
        // through it, so nothing behind it becomes unclickable.
        constexpr ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
        ImGui::Begin("##netgraph", nullptr, windowFlags);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float graphHeight = MainLaneHeight + ResimLaneHeight + EventLaneHeight + LaneGap * 2.0f;
        ImGui::Dummy(ImVec2(GraphWidth, graphHeight));

        const float mainTop = origin.y;
        const float mainBottom = mainTop + MainLaneHeight;
        const float resimTop = mainBottom + LaneGap;
        const float resimBottom = resimTop + ResimLaneHeight;
        const float eventTop = resimBottom + LaneGap;
        const float eventBottom = eventTop + EventLaneHeight;
        const float rightEdge = origin.x + GraphWidth;

        draw->AddRectFilled(ImVec2(origin.x, mainTop), ImVec2(rightEdge, mainBottom), LaneBackground);
        draw->AddRectFilled(ImVec2(origin.x, resimTop), ImVec2(rightEdge, resimBottom), LaneBackground);
        draw->AddRectFilled(ImVec2(origin.x, eventTop), ImVec2(rightEdge, eventBottom), LaneBackground);

        // A line per second, anchored to the newest column, so a burst can be placed in time without
        // counting columns.
        for (int column = static_cast<int>(Length) - GridSpacing; column > 0; column -= GridSpacing) {
            const float x = origin.x + static_cast<float>(column) * ColumnWidth;
            draw->AddLine(ImVec2(x, mainTop), ImVec2(x, resimBottom), GridLine);
        }
        // Where a correction stops being tolerated and starts costing a replay: bars under this line
        // are predictions that were already good enough to keep.
        if (!stats.hosting && stats.toleranceMetres > 0.0f && stats.toleranceMetres < correctionScale) {
            const float y = mainBottom - stats.toleranceMetres / correctionScale * MainLaneHeight;
            draw->AddLine(ImVec2(origin.x, y), ImVec2(rightEdge, y), ToleranceLine);
        }

        for (size_t column = 0; column < Length; ++column) {
            const NetGraphSample& sample = at(column);
            const float x = origin.x + static_cast<float>(column) * ColumnWidth;

            if (stats.hosting) {
                if (sample.bytesSent > 0) {
                    drawBar(draw, x, mainBottom, MainLaneHeight,
                            static_cast<float>(sample.bytesSent) / byteScale, SentColor);
                }
                if (sample.bytesReceived > 0) {
                    drawBar(draw, x, resimBottom, ResimLaneHeight,
                            static_cast<float>(sample.bytesReceived) / byteScale, ReceivedColor);
                }
            } else {
                if ((sample.flags & NetGraphApplied) != 0) {
                    const ImU32 color = (sample.flags & NetGraphSnapped) != 0 ? SnapColor
                                      : (sample.flags & NetGraphReplayed) != 0 ? ReplayColor
                                      : KeptColor;
                    drawBar(draw, x, mainBottom, MainLaneHeight, sample.correction / correctionScale, color);
                }
                if (sample.resimDepth > 0) {
                    drawBar(draw, x, resimBottom, ResimLaneHeight,
                            static_cast<float>(sample.resimDepth) / ResimScaleTicks, ResimColor);
                }
            }

            // The strip under both lanes is for things that either happened or did not, where a
            // height would mean nothing. Loss wins the cell: it is the one worth spotting.
            const ImU32 event = (sample.flags & NetGraphLost) != 0 ? LostColor
                              : (sample.flags & NetGraphStarved) != 0 ? StarvedColor
                              : (sample.flags & NetGraphApplied) != 0 ? AppliedColor
                              : 0;
            if (event != 0) {
                draw->AddRectFilled(ImVec2(x, eventTop), ImVec2(x + ColumnWidth, eventBottom), event);
            }
        }

        char label[32];
        if (stats.hosting) SDL_snprintf(label, sizeof(label), "%.0f B/tick", byteScale);
        else SDL_snprintf(label, sizeof(label), "%.2f m", correctionScale);
        draw->AddText(ImVec2(origin.x + 4.0f, mainTop + 2.0f), ScaleLabel, label);
        if (stats.hosting) SDL_snprintf(label, sizeof(label), "in %.0f B", byteScale);
        else SDL_snprintf(label, sizeof(label), "%.0f ticks", ResimScaleTicks);
        draw->AddText(ImVec2(origin.x + 4.0f, resimTop + 1.0f), ScaleLabel, label);

        if (!showStats) {
            ImGui::End();
            return;
        }

        if (stats.hosting) {
            legendEntry("sent", SentColor, true);
            legendEntry("received", ReceivedColor);
        } else {
            legendEntry("kept", KeptColor, true);
            legendEntry("replayed", ReplayColor);
            legendEntry("snapped", SnapColor);
            legendEntry("resim", ResimColor);
            legendEntry("lost", LostColor);
            legendEntry("starved", StarvedColor);
        }

        ImGui::Separator();

        if (stats.pingMs < 0) {
            ImGui::TextDisabled("Ping --");
        } else {
            const ImVec4 pingColor = stats.pingMs < 60 ? ImVec4(0.35f, 0.8f, 0.45f, 1.0f)
                                   : stats.pingMs < 120 ? ImVec4(0.95f, 0.75f, 0.3f, 1.0f)
                                   : ImVec4(0.92f, 0.35f, 0.3f, 1.0f);
            ImGui::TextColored(pingColor, "Ping %d ms", stats.pingMs);
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (stats.lossPct < 0.0f) ImGui::TextDisabled("| loss n/a");
        else ImGui::Text("| loss %.1f%%", stats.lossPct);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::Text("| %.1f/%.1f KB/s", stats.inKbPerSec, stats.outKbPerSec);
        if (stats.simulated) {
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "| sim");
        }

        if (stats.hosting) {
            ImGui::Text("Host | %d client%s | %d entities", stats.connections,
                        stats.connections == 1 ? "" : "s", stats.replicatedEntities);
            ImGui::End();
            return;
        }

        // The delay is the whole of the local number: input read this frame is filed for a tick that
        // far ahead, and this ball does not act on it until then. Bought deliberately, in exchange
        // for every other peer simulating it from what was pressed rather than guessing.
        ImGui::Text("Input lag %.0f ms (%dt) | lead %d/%d | hold %dt (%.0f ms)",
                    stats.inputLagMs, stats.inputDelayTicks, stats.inputLeadTicks, stats.inputLeadTarget,
                    stats.jitterHoldTicks, stats.jitterHoldMs);
        // Age is how far in the past the host's newest word is; correction is how wrong we turned out
        // to be about that gap. Near-zero correction at a large age is the prediction doing its job.
        ImGui::Text("State age %.0f ms | correction %.3f/%.3f m avg/peak",
                    stats.stateAgeMs, stats.correctionMean, stats.correctionPeak);
        if (stats.keptPct < 0.0f) {
            ImGui::TextDisabled("Awaiting first snapshot");
        } else {
            ImGui::Text("Resim %.0f/s | %dt deep | kept %.0f%% | lost %d | starved %d",
                        stats.resimStepsPerSecond, stats.deepestResim, stats.keptPct,
                        stats.lostSnapshots, stats.starvedInputs);
        }

        ImGui::End();
    }
} // ytail::net
