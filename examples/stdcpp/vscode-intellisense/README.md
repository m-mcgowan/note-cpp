# note-cpp IntelliSense demo

Open this folder as a VS Code workspace and try out auto-completion
and hover docs on the typed Notecard API.

## One-time setup

1. Install the **C/C++** extension (`ms-vscode.cpptools`) if you don't
   already have it. VS Code will offer it as a recommendation when you
   open this folder.
2. From a terminal in this directory, generate a `compile_commands.json`:

   ```sh
   cmake -B build
   ```

   That's the only build step needed for the editor — IntelliSense
   reads the include paths, defines, and `-std=c++20` from
   `build/compile_commands.json`. The `.vscode/c_cpp_properties.json`
   in this folder points the C/C++ extension at it.

3. Open `main.cpp`. IntelliSense indexes in the background; once it
   finishes (a few seconds), hovers and completion light up.

## What to try

`main.cpp` has six annotated spots. Put your cursor where each
"Auto-complete spot N" comment indicates and press **Ctrl+Space** (or
**Cmd+Space** on macOS) to trigger completion. Hover over fields and
methods to see the doc comments pulled from the Notecard API spec.

## If completion isn't working

| Symptom | Fix |
|---|---|
| `<note/...>` headers shown red | Re-run `cmake -B build` and reload the window — the C/C++ extension needs `compile_commands.json` to exist. |
| Completion is sluggish on first hover | Initial parse takes a few seconds because note-cpp has many template-heavy headers. Subsequent edits should be fast. |
| Stale completions after editing headers | `Cmd+Shift+P` → **C/C++: Reset IntelliSense Database**. |

## Using this layout in your own project

The minimum to get the same experience for any C++ project that uses
note-cpp:

```cmake
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
target_include_directories(my_target PRIVATE path/to/note-cpp/include)
```

Then in `.vscode/c_cpp_properties.json` set
`"compileCommands": "${workspaceFolder}/build/compile_commands.json"`.
