# ImGuizmo (vendored)

Immediate-mode 3D transform gizmos for the editor. Draws through ImGui's draw list, so it
needs no render pass of its own.

- Upstream: https://github.com/CedricGuillemet/ImGuizmo
- Commit: `fd138a7ce089cfcbfee16da8e8692f37a0e9481e` (2026-07-09; header self-reports "v1.92.5 WIP")
- License: MIT, in the comment block at the top of each file.

Vendored rather than fetched. The newest upstream tag is 1.83 (2021), so a SHA is the only
usable pin, and upstream's CMakeLists fetches imgui at `GIT_TAG master` and declares its own
`imgui` target, which would collide with ours and put a second ImGui in the binary.
`ImGuizmo.cpp` is compiled straight into `ytail_editor` (see `src/CMakeLists.txt`) so it sees
our imgui and nothing else.

Only `ImGuizmo.h` and `ImGuizmo.cpp` are taken. GraphEditor, ImCurveEdit, ImGradient,
ImSequencer, ImVectorEditor, ImZoomSlider and ImLightRig are unused.

Both files are byte-identical to upstream, so local edits show up in a diff:

    SHA=fd138a7ce089cfcbfee16da8e8692f37a0e9481e
    curl -fsSL https://raw.githubusercontent.com/CedricGuillemet/ImGuizmo/$SHA/src/ImGuizmo.cpp | diff - ImGuizmo.cpp

To update: bump the SHA, re-download both files, update this file.
