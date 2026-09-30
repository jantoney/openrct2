/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../Identifiers.h"
#include "../world/Location.hpp"

#include <set>
#include <tuple>
#include <vector>

using Direction = uint8_t;
struct TileCoordsXYZ;

namespace OpenRCT2
{
    struct Guest;
    struct Peep;
} // namespace OpenRCT2

namespace OpenRCT2::PathFinding
{
    struct ArrivalPathSearch
    {
        std::vector<TileCoordsXYZ> pending;
        std::set<std::tuple<int32_t, int32_t, int32_t>> visited;
        uint32_t processed{};
        uint32_t lastUpdateTick{ UINT32_MAX };
        bool foundOwnedBoundary{};
    };

    Direction ChooseDirection(
        const TileCoordsXYZ& loc, const TileCoordsXYZ& goal, Peep& peep, bool ignoreForeignQueues, RideId queueRideIndex);

    int32_t CalculateNextDestination(Guest& peep);

    bool IsArrivalTrappedByOwnedPaths(const Guest& peep);

    // Call after authoritative changes to paths, ownership, entrances, or spawn points.
    void InvalidateArrivalPathSearches();

    int32_t GuestPathFindParkEntranceEntering(Peep& peep, uint8_t edges);

    int32_t GuestPathFindPeepSpawn(Peep& peep, uint8_t edges);

    int32_t GuestPathFindParkEntranceLeaving(Peep& peep, uint8_t edges);

} // namespace OpenRCT2::PathFinding
