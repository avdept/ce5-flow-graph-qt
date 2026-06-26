# FlowGraph Qt

A DPI-correct Qt FlowGraph editor for the CRYENGINE 5.7 Sandbox, built on the
existing `CryGraphEditor` NodeGraph framework. It renders and edits the same
`CHyperGraph`/`CFlowGraphManager` graphs as the legacy MFC HyperGraph editor,
which doesn't scale on high-DPI displays.

Registers a pane under **Tools -> "FlowGraph Qt"**, alongside the legacy editor.

By default engine still uses old MFC based FlowGraph, so you need to open it via toolbar menu and select your flowgraph from sidebar

It still misses debugging capabilities and few other minor things which I plan to add shortly, when I need them.

I also added tab menu - to mimic UnrealEngine one, that opens you dropdown and allows to search for needed node right away

## Integrating into a CRYENGINE 5.7 source tree

1. Copy `src/*.{h,cpp}` to `Code/Sandbox/EditorQt/HyperGraph/Qt/`.

2. Apply the patches from the repo root (`git apply FlowGraphQt/patches/*.patch`,
   or apply by hand - they're a few lines each):
   - `01-HeaderWidget-skip-empty-icon` - skip the node title-icon slot when the
     style has no icon (gives the flush-left title).
   - `02-PinWidget-style-text-color` - read the pin-name color/font from the
     style instead of a hardcoded grey.
   - `03-FlowGraphNode-public-GetEntityTitle` - make `CFlowNode::GetEntityTitle()`
     public so the entity bar can show it.

3. Register the sources: inside the existing
   `add_sources("Editor_Uber_HyperGraph.cpp" ...)` block in
   `Code/Sandbox/EditorQt/CMakeLists.txt`, add:

   ```cmake
   SOURCE_GROUP "HyperGraph\\\\Qt"
       "HyperGraph/Qt/FlowGraphQtViewModel.h"
       "HyperGraph/Qt/FlowGraphQtViewModel.cpp"
       "HyperGraph/Qt/FlowGraphQtNodeProperties.h"
       "HyperGraph/Qt/FlowGraphQtNodeProperties.cpp"
       "HyperGraph/Qt/FlowGraphQtDockable.h"
       "HyperGraph/Qt/FlowGraphQtDockable.cpp"
   ```

4. Regenerate the solution and build the `Sandbox` target.

## The Tools menu entry

No manual menu wiring is needed. This line at the top of
`FlowGraphQtDockable.cpp` registers the pane with Sandbox:

```cpp
REGISTER_VIEWPANE_FACTORY(CFlowGraphQtDockable, "FlowGraph Qt", "Tools", false)
```

Once Sandbox is built, the editor shows up as **Tools -> FlowGraph Qt** in the
top menu bar. Change the 2nd argument to rename it, or the 3rd to move it to a
different top-level menu.

## Dependencies

Sandbox-only (`EditorCommon` NodeGraph framework, `CryQt`) plus the legacy
HyperGraph data model (`CHyperGraph`, `CHyperFlowGraph`, `CFlowGraphManager`,
`CFlowNode`). No gameplay/runtime code.

## Build target

VS2022 generator + v141 toolset, Qt 5.12.3 (the 5.7 SDK default). Profile config.
