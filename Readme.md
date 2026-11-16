![Spotify Status](/assets/card.svg)

# ReZeroDetour

A lightweight inline hook library for Windows x64.
Built for learning purposes. Uses Zydis for disassembly.

## Features

-  No CRT heap dependency
-  Zydis decoder + encoder(fetched via CMake FetchContent)
-  Stubs and jumps are emitted by `CodeGen` (a thin Zydis encoder wrapper),
   not hand-written opcodes; every emit returns its instruction length

## Requirements

- Windows x64
- CMake
- A C++23 compiler (MSVC, Clang or GCC)

## Quick Start
```cpp
#include <ReZeroDetour/ReZeroDetour.h>
using namespace ReZeroDetour;

int add(int a, int b) { return a + b; }  
int main() {
    auto *hook = makeInlineHook(&add, [](auto &self, int a, int b) -> int {
        return self.trampoline(a, b) * 10;   
    });
    if (hook == nullptr) return 1; 
    hook->mode(HookMode::Replace);

    if (!hook->install()) return 2;
    add(3, 4);            // -> 70
    hook->uninstall(); 
}

## Bulk uninstall

Patches are tracked in a non-template `PatchTable` (target address + original
bytes), so the byte layer can be cleaned up in one call, without touching any
hook object:

```cpp
auto report = ReZeroDetour::uninstallAllPatches();   // 默认表，全部还原
if (report.HasError) { /* report.FirstError, report.Failed */ }
```

Hooks are still uninstalled individually (draining in-flight calls needs the
per-hook counter), so call `hook->uninstall()` for each hook and use the table
only as the byte-level safety net.
