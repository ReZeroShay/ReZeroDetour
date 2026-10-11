![Spotify Status](/assets/card.svg)

# ReZeroDetour

A lightweight inline hook library for Windows x64.
Built for learning purposes. Uses Zydis for disassembly.

## Features

-  No CRT heap dependency
-  Zydis(automatically fetched via CMake FetchContent)

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
