# Mergerino code guidelines

This document captures the coding conventions used in Mergerino. The goal is to make the codebase approachable for contributors coming from other languages or frameworks while keeping style decisions consistent.

## General guidelines

- Work on topic branches instead of committing directly to your long-lived branch.
- Prefer clear, maintainable code over clever shortcuts.
- Keep comments for context that is not obvious from the code itself.

## Formatting

Code is formatted with `clang-format`. Keep automatic formatting enabled in your editor if possible.

## Naming

- Functions use `camelCase`
- Private members use `camelCase_`
- Getters do not use a `get` prefix unless there is a strong reason

## Resource management

- Prefer stack allocation where ownership is simple
- Use `std::unique_ptr` for single-owner heap allocations
- Use `std::shared_ptr` only when shared ownership is required
- For `QObject` types, prefer Qt parent ownership or `deleteLater()`
