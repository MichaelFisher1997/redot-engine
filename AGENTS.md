# Redot Engine Agent Guidelines

## Build Commands

### Build the engine
```bash
# Build editor (default target)
scons platform=linuxbsd target=editor

# Build debug template
scons platform=linuxbsd target=template_debug

# Build release template
scons platform=linuxbsd target=template_release

# With custom flags (example: dev build with sanitizers)
scons platform=linuxbsd target=editor dev_build=yes use_asan=yes
```

### Run tests
```bash
# Run all tests
./bin/redot.linuxbsd.editor.x86_64 --headless --test --force-colors

# Run specific test by name (doctest filter)
./bin/redot.linuxbsd.editor.x86_64 --headless --test --test-filter "TestCaseName"
```

### Linting and formatting
```bash
# Run all pre-commit checks on staged files
pre-commit run

# Run on all files
pre-commit run --all-files

# Run specific hook
pre-commit run clang-format --all-files
pre-commit run ruff --all-files

# Manual clang-tidy (not auto-run)
pre-commit run --hook-stage manual clang-tidy
```

## Code Style Guidelines

### File structure
- All source files must include MIT license header (31-line comment block)
- Header files use `#pragma once` instead of include guards
- File naming: `snake_case.cpp` / `snake_case.h` (e.g., `print_string.cpp`)
- Test files: `test_<name>.h` in `tests/` directory (e.g., `test_color.h`)

### Indentation and formatting
- **Language**: C++20 standard
- **Indentation**: Tabs with 4-space tab width
- **Pointer alignment**: Right-aligned (`Type *ptr`)
- **Brace style**: Attached (LLVM style)
- **Column limit**: None (0)
- **Always insert braces**: Required for control statements
- Use clang-format to auto-format: `clang-format -i file.cpp`

### Naming conventions
- **Classes**: PascalCase (e.g., `PrintString`, `CoreGlobals`)
- **Functions/Methods**: snake_case (e.g., `add_print_handler`, `_global_lock`)
- **Local variables**: snake_case (e.g., `print_handler_list`, `is_printing`)
- **Class members**: snake_case, private members prefixed with `_` (e.g., `_member`)
- **Constants/Statics**: UPPER_SNAKE_CASE (e.g., `print_line_enabled`, `LEAK_REPORTING_ENABLED`)
- **Macros**: UPPER_SNAKE_CASE, engine macros use specific prefixes:
  - `_ALWAYS_INLINE_`, `_FORCE_INLINE_`, `_NO_INLINE_` for inlining
  - `ERR_FAIL_COND`, `ERR_FAIL`, `ERR_CONTINUE` for error handling
- **Namespaces**: PascalCase (e.g., `TestColor`)

### Testing conventions
- Test cases defined with `TEST_CASE("[Component] Description")`
- Use namespace `Test<ClassName>` for tests (e.g., `namespace TestColor`)
- Available macros: `TEST_CASE`, `TEST_CASE_PENDING`, `TEST_CASE_MAY_FAIL`
- Assertion macros: `TEST_COND`, `TEST_FAIL`, `TEST_FAIL_COND`, `TEST_FAIL_COND_WARN`
- For error path testing: Use `ERR_PRINT_OFF` / `ERR_PRINT_ON` macros
- Create new test: `python tests/create_test.py ClassName path/to/test`

### Error handling
- Use engine's error macros: `ERR_FAIL_COND`, `ERR_FAIL`, `ERR_FAIL_NULL`, `ERR_CONTINUE`
- Check `core/error/error_macros.h` for available error macros
- Return early on failure conditions common in codebase

### Include order
1. Local headers (e.g., `#include "print_string.h"`)
2. Core headers (e.g., `#include "core/core_globals.h"`)
3. System headers (e.g., `#include <stdio.h>`)

Includes are sorted by clang-format automatically.

### Code patterns
- Static class members defined inline: `static inline bool flag = true;`
- Global functions for thread-safe operations: `_global_lock()`, `_global_unlock()`
- String conversion: Use `.utf8().get_data()` for C-string access
- Use `thread_local` for thread-specific data where needed

### Module structure
- Core functionality: `core/` directory
- Editor code: `editor/` directory
- Platform-specific: `platform/<name>/` directory
- Optional features: `modules/<name>/` directory
- Tests: `tests/<category>/` directory (core, scene, servers, etc.)

### Build files
- Build configuration: `SConstruct` (root) and `SCsub` (per directory)
- Python scripts for generators: `*_builders.py` pattern
- Module config: `modules/<name>/config.py`

### Additional guidelines
- Use existing patterns from similar code when adding new features
- Check existing headers/macros before creating new ones
- Tests should be added with bug fixes and new features
- Documentation updates required for API changes (XML files in `doc/classes/`)
- Pre-commit must pass before PR submission
- Commit messages: Title <72 chars, imperative mood, use prefixes (Core:, Add:, Fix:, etc.)
