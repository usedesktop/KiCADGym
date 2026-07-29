# KiCad README

## KiCadGym release identity

KiCadGym package `1.0.0` tracks the official KiCad `10.0` stable maintenance line. Its current
source base is KiCad `10.0.5` plus stable maintenance commits at
`8709bb1f26e3e088c71b600df9c413ff89554612`. The package manifest records the integration version
and upstream application identity separately. Development builds must not silently track KiCad
`master` or infer a version from a local executable.

The Windows install tree is self-contained. In addition to KiCad and vcpkg runtime DLLs it ships
the embedded Python 3.11 executable, standard library, encodings, extension DLLs, and
`site-packages`. This is required because stable KiCad initializes Python relative to its installed
executable directory.

Use `kicad-cli version` to verify the application build. GUI executables such as `pcbnew.exe` are
not CLI probes and must not be started with `--help`.

The stable Windows configure requires SWIG 4 or newer. The current workspace uses the SWIG binary
from FreeCADGym's Pixi environment and configures with `VCPKG_MANIFEST_INSTALL=OFF` against the
already provisioned vcpkg tree.

## KiCadGym native action capture

KiCadGym emits `rl_env.native_action.v1` JSONL through the common RL environment contract:

- `RL_ENV_KIND=kicadgym`
- `RL_ENV_SESSION_ID=<session id>`
- `RL_ENV_NATIVE_ACTION_CONTROL_FILE=<control file containing the active JSONL path>`

A global wxWidgets event filter captures clickable controls and editable properties. The tool
manager records completed KiCad commands, while schematic and board commits record explicitly
linked artifact transactions. Desktop owns recording boundaries, checkpoints, semantic grouping,
and verifier/reward authoring.

The visible-action catalog covers menus, tools, buttons, text and numeric inputs, choices,
property-grid rows, grids, data views, trees, tabs, and canvas pointer/wheel interactions. The
semantic matrix includes PCB route/drag/tuning/zone/via/track/footprint/pad/rules/DRC, schematic
symbols/properties/connectivity/hierarchy/ERC/simulation/footprint assignment, Symbol Editor,
Footprint Editor, Gerber Viewer, 3D Viewer, Page Layout Editor, Image Converter, Calculator,
fabrication export, project jobs, and plugins. Each action exposes stable IDs and verifier
bindings, while board and schematic commits carry an explicit parent interaction ID.

Native regression coverage lives in
`qa/tests/common/test_kicadgym_native_action_catalog.cpp`. It checks the editor/canvas semantic
matrix and writes a real routed-command plus board-commit JSONL trace. Desktop's
`scripts/smoke-local-rl-env-packages.mjs --kicadgym-only` launches the installed package and
checks the live catalog, verifier execution, snapshot, reset, and recording artifact binding.
`scripts/smoke-kicadgym-editor-catalog.mjs` then launches every standalone editor exposed by the
KiCad manager and checks that each one publishes non-empty `kicadgym.ui_state.v1` with stable
catalog, instance, and semantic IDs plus catalog/visible/enabled/invoked verifier bindings. Run it
from `desktop` with `npm run smoke:kicadgym-editors` after installing a native build.

The common native capture filter republishes the visible-action catalog after wxWidgets finishes
showing and laying out any window. This keeps menus, panels, tabs, and dialogs current without
per-editor timers or application-specific startup hooks.

For specific documentation about [building KiCad](https://dev-docs.kicad.org/en/build/), policies
and guidelines, and source code documentation see the
[Developer Documentation](https://dev-docs.kicad.org) website.

You may also take a look into the [Wiki](https://gitlab.com/kicad/code/kicad/-/wikis/home),
the [contribution guide](https://dev-docs.kicad.org/en/contribute/).

For general information about KiCad and information about contributing to the documentation and
libraries, see our [Website](https://kicad.org/) and our [Forum](https://forum.kicad.info/).

## Build state

KiCad uses a host of CI resources.

GitLab CI pipeline status can be viewed for Linux and Windows builds of the latest commits.

## Release status
[![latest released version(s)](https://repology.org/badge/latest-versions/kicad.svg)](https://repology.org/project/kicad/versions)
[![Release status](https://repology.org/badge/tiny-repos/kicad.svg)](https://repology.org/metapackage/kicad/versions)

## Files
* [AUTHORS.txt](AUTHORS.txt) - The authors, contributors, document writers and translators list
* [CMakeLists.txt](CMakeLists.txt) - Main CMAKE build tool script
* [copyright.h](copyright.h) - A very short copy of the GNU General Public License to be included in new source files
* [Doxyfile](Doxyfile) - Doxygen config file for KiCad
* [INSTALL.txt](INSTALL.txt) - The release (binary) installation instructions
* [uncrustify.cfg](uncrustify.cfg) - Uncrustify config file for uncrustify sources formatting tool
* [_clang-format](_clang-format) - clang config file for clang-format sources formatting tool

## Subdirectories

* [3d-viewer](3d-viewer)         - Sourcecode of the 3D viewer
* [bitmap2component](bitmap2component)  - Sourcecode of the bitmap to PCB artwork converter
* [cmake](cmake)      - Modules for the CMAKE build tool
* [common](common)            - Sourcecode of the common library
* [cvpcb](cvpcb)             - Sourcecode of the CvPCB tool
* [demos](demos)             - Some demo examples
* [doxygen](doxygen)     - Configuration for generating pretty doxygen manual of the codebase
* [eeschema](eeschema)          - Sourcecode of the schematic editor
* [gerbview](gerbview)          - Sourcecode of the gerber viewer
* [include](include)           - Interfaces to the common library
* [kicad](kicad)             - Sourcecode of the project manager
* [libs](libs)           - Sourcecode of KiCad utilities (geometry and others)
* [pagelayout_editor](pagelayout_editor) - Sourcecode of the pagelayout editor
* [patches](patches)           - Collection of patches for external dependencies
* [pcbnew](pcbnew)           - Sourcecode of the printed circuit board editor
* [plugins](plugins)           - Sourcecode for the 3D viewer plugins
* [qa](qa)                - Unit testing framework for KiCad
* [resources](resources)         - Packaging resources such as bitmaps and operating system specific files
    - [bitmaps_png](resources/bitmaps_png)       - Menu and program icons
    - [project_template](resources/project_template)          - Project template
* [scripting](scripting)         - Python integration for KiCad
* [thirdparty](thirdparty)           - Sourcecode of external libraries used in KiCad but not written by the KiCad team
* [tools](tools)             - Helpers for developing, testing and building
* [translation](translation) - Translation data files (managed through [Weblate](https://hosted.weblate.org/projects/kicad/master-source/) for most languages)
* [utils](utils)             - Small utils for KiCad, e.g. IDF, STEP, and OGL tools and converters
