// GameVault for PS5 - The app's screens, as the UI kit's "designs".
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replaces the kit's src/concepts/concepts.hpp in the build: the kit's shell
// runs whatever registry.cpp lists. GameVault is one design that holds its
// own sections (Library, Downloads, Storage, Settings), switched with L1/R1.

#pragma once

#include "app/concept.hpp"

#include <memory>

namespace hui::concepts
{

std::unique_ptr<app::Concept> make_gamevault(app::Context &context);

} // namespace hui::concepts
