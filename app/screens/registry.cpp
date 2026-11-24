// GameVault for PS5 - The one design the kit's shell runs.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/concepts.hpp"

namespace hui::app
{

std::span<const ConceptFactory> concept_registry()
{
    static const ConceptFactory kRegistry[] = {
        concepts::make_gamevault,
    };
    return kRegistry;
}

} // namespace hui::app
