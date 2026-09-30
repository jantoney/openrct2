#include "TestData.h"

#include <gtest/gtest.h>
#include <memory>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/footpath/FootpathRemoveAction.h>
#include <openrct2/actions/general/GameSetSpeedAction.h>
#include <openrct2/actions/general/PauseToggleAction.h>
#include <openrct2/actions/park/LandSetRightsAction.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/core/String.hpp>
#include <openrct2/entity/Guest.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/peep/GuestPathfinding.h>
#include <openrct2/platform/Platform.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/scenario/Scenario.h>
#include <openrct2/world/Entrance.h>
#include <openrct2/world/Footpath.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <string>

using namespace OpenRCT2;

static std::ostream& operator<<(std::ostream& os, const TileCoordsXYZ& coords)
{
    return os << "(" << coords.x << ", " << coords.y << ", " << coords.z << ")";
}

class PathfindingTestBase : public testing::Test
{
public:
    static void SetUpTestCase()
    {
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        _context = CreateContext();
        const bool initialised = _context->Initialise();
        ASSERT_TRUE(initialised);

        std::string parkPath = TestData::GetParkPath("pathfinding-tests.sv6");
        GetContext()->LoadParkFromFile(parkPath);
        GameLoadInit(); // NB: calls `setActiveScene`
    }

    void SetUp() override
    {
        // Use a consistent random seed in every test
        ScenarioRandSeed(0x12345678, 0x87654321);
    }

    static void TearDownTestCase()
    {
        _context = nullptr;
    }

protected:
    static Ride* FindRideByName(const char* name)
    {
        auto& gameState = getGameState();
        for (auto& ride : RideManager(gameState))
        {
            auto thisName = ride.getName();
            if (String::startsWith(thisName, u8string{ name }, true))
            {
                return &ride;
            }
        }
        return nullptr;
    }

    static bool FindPath(TileCoordsXYZ* pos, const TileCoordsXYZ& goal, int expectedSteps, RideId targetRideID)
    {
        // Our start position is in tile coordinates, but we need to give the peep spawn
        // position in actual world coords (32 units per tile X/Y, 8 per Z level).
        // Add 16 so the peep spawns in the centre of the tile.
        auto* peep = Guest::generate(pos->toCoordsXYZ().toTileCentre());

        // Peeps that are outside of the park use specialized pathfinding which we don't want to
        // use here
        peep->outsideOfPark = false;

        // An earlier iteration of this code just gave peeps a target position to walk to, but it turns out
        // that with no actual ride to head towards, when a peep reaches a junction they use the 'aimless'
        // pathfinder instead of pursuing their original pathfinding target. So, we always need to give them
        // an actual ride to walk to the entrance of.
        peep->guestHeadingToRideId = targetRideID;

        // Pick the direction the peep should initially move in, given the goal position.
        // This will also store the goal position and initialize pathfinding data for the peep.
        const Direction moveDir = PathFinding::ChooseDirection(*pos, goal, *peep, false, RideId::GetNull());
        if (moveDir == kInvalidDirection)
        {
            // Couldn't determine a direction to move off in
            return false;
        }

        // We have already set up the peep's overall pathfinding goal, but we also have to set their initial
        // 'destination' which is a close position that they will walk towards in a straight line - in this case, one
        // tile away. Stepping the peep will move them towards their destination, and once they reach it, a new
        // destination will be picked, to try and get the peep towards the overall pathfinding goal.
        peep->peepDirection = moveDir;
        auto destination = CoordsDirectionDelta[moveDir] + peep->getLocation();
        peep->setDestination(destination, 2);

        // Repeatedly step the peep, until they reach the target position or until the expected number of steps have
        // elapsed. Each step, check that the tile they are standing on is not marked as forbidden in the test data
        // (red neon ground type).
        int step = 0;
        while (*pos != goal && step < expectedSteps)
        {
            peep->performNextAction();
            ++step;

            *pos = TileCoordsXYZ(peep->getLocation());

            EXPECT_PRED_FORMAT1(AssertIsNotForbiddenPosition, *pos);

            // Check that the peep is still on a footpath. Use next_z instead of pos->z here because pos->z will change
            // when the peep is halfway up a slope, but next_z will not change until they move to the next tile.
            EXPECT_NE(MapGetFootpathElement({ pos->toCoordsXY(), peep->nextLoc.z }), nullptr);
        }

        // Clean up the peep, because we're reusing this loaded context for all tests.
        PeepEntityRemove(peep);

        // Require that the number of steps taken is exactly what we expected. The pathfinder is supposed to be
        // deterministic, and we reset the RNG seed for each test, everything should be entirely repeatable; as
        // such a change in the number of steps taken on one of these paths needs to be reviewed. For the negative
        // tests, we will not have reached the goal but we still expect the loop to have run for the total number
        // of steps requested before giving up.
        EXPECT_EQ(step, expectedSteps);

        return *pos == goal;
    }

    static testing::AssertionResult AssertIsStartPosition(const char*, const TileCoordsXYZ& location)
    {
        const auto expectedSurfaceColour = EnumValue(Drawing::Colour::brightGreen);
        const auto colour = EnumValue(MapGetSurfaceElementAt(location.toCoordsXYZ())->getPrimarySurfaceColour());

        if (colour != expectedSurfaceColour)
            return testing::AssertionFailure()
                << "Start location " << location << " should have surface colour " << expectedSurfaceColour
                << " but actually has style " << colour
                << ". Either the test map is not set up correctly, or you got the coordinates wrong.";

        return testing::AssertionSuccess();
    }

    static testing::AssertionResult AssertIsNotForbiddenPosition(const char*, const TileCoordsXYZ& location)
    {
        const uint32_t forbiddenSurfaceColour = EnumValue(Drawing::Colour::brightRed);
        const auto colour = EnumValue(MapGetSurfaceElementAt(location.toCoordsXYZ())->getPrimarySurfaceColour());

        if (colour == forbiddenSurfaceColour)
            return testing::AssertionFailure()
                << "Path traversed location " << location << ", but it is marked as a forbidden location (surface colour "
                << forbiddenSurfaceColour << "). Either the map is set up incorrectly, or the pathfinder went the wrong way.";

        return testing::AssertionSuccess();
    }

private:
    static std::shared_ptr<IContext> _context;
};

std::shared_ptr<IContext> PathfindingTestBase::_context;

TEST_F(PathfindingTestBase, ArrivingGuestCanWalkOnPurchasedApproachPath)
{
    const TileCoordsXYZ start{ 19, 15, 14 };
    auto* ride = FindRideByName("StraightFlat");
    ASSERT_NE(ride, nullptr);
    const auto entrance = ride->getStation().entrance;
    const TileCoordsXYZ goal{ entrance.x - TileDirectionDelta[entrance.direction].x,
                              entrance.y - TileDirectionDelta[entrance.direction].y, entrance.z };
    auto* guest = Guest::generate(start.toCoordsXYZ().toTileCentre());
    ASSERT_NE(guest, nullptr);
    guest->state = PeepState::enteringPark;
    guest->var37 = 0;
    guest->nextLoc = start.toCoordsXYZ();
    const auto direction = PathFinding::ChooseDirection(start, goal, *guest, false, RideId::GetNull());
    ASSERT_NE(direction, kInvalidDirection);
    const auto destination = CoordsDirectionDelta[direction] + guest->getLocation();
    auto* surface = MapGetSurfaceElementAt(destination);
    ASSERT_NE(surface, nullptr);
    const auto oldOwnership = surface->getOwnership();
    surface->setOwnership({ OwnershipFlag::landOwned });
    guest->peepDirection = direction;
    guest->setDestination(destination, 2);

    bool leftStartTile = false;
    for (int step = 0; step < 100; ++step)
    {
        guest->performNextAction();
        if (TileCoordsXYZ(guest->getLocation()) != start)
        {
            leftStartTile = true;
            break;
        }
    }
    EXPECT_TRUE(leftStartTile);
    EXPECT_TRUE(guest->outsideOfPark);
    surface->setOwnership(oldOwnership);
    guest->remove();
}

class EnclosedArrivalTest : public PathfindingTestBase
{
protected:
    void SetUp() override
    {
        PathfindingTestBase::SetUp();
        MapInit({ 32, 32 }, Drawing::Colour::brightGreen);
        auto& gameState = getGameState();
        gameState.park.entrances.clear();
        gameState.peepSpawns.clear();
        gameState.peepSpawns.push_back({ 32, 160, 112, 0 });
        gameState.park.flags.set(ParkFlag::parkOpen);
        gameState.park.numGuestsInPark = 10;
        gameState.park.numGuestsHeadingForPark = 0;
        gameState.park.totalAdmissions = 10;
        for (int x = 1; x <= 10; ++x)
        {
            const CoordsXYZ loc{ x * 32, 160, 112 };
            auto* surface = MapGetSurfaceElementAt(loc);
            surface->setBaseZ(112);
            surface->setClearanceZ(112);
            surface->setOwnership(x >= 3 ? OwnershipFlags{ OwnershipFlag::landOwned } : kUnowned);
            auto* path = TileElementInsert<PathElement>(loc, 0);
            ASSERT_NE(path, nullptr);
            path->setClearanceZ(128);
            path->setEdges(x == 10 ? 1 : 5);
        }
    }

    static Guest* CreateArrival()
    {
        auto* guest = Guest::generate({ 272, 176, 112 });
        guest->state = PeepState::enteringPark;
        guest->var37 = 0;
        guest->nextLoc = { 256, 160, 112 };
        guest->setNextFlags(0, false, false);
        guest->peepDirection = 0;
        guest->setDestination({ 240, 176 }, 2);
        return guest;
    }

    static void AddExit(int x)
    {
        const CoordsXYZ loc{ x * 32, 160, 112 };
        TileElementRemove(reinterpret_cast<TileElement*>(MapGetFootpathElement(loc)));
        auto* entrance = TileElementInsert<EntranceElement>(loc, 0);
        ASSERT_NE(entrance, nullptr);
        entrance->setClearanceZ(168);
        entrance->setEntranceType(EntranceType::parkEntrance);
        entrance->setSequenceIndex(ParkEntranceSequence::centre);
        entrance->setDirection(2);
        getGameState().park.entrances.push_back({ loc, 2 });
    }

    static void TickGuest(EntityId id, int ticks)
    {
        for (int i = 0; i < ticks; ++i)
        {
            auto* guest = getGameState().entities.getEntity<Guest>(id);
            if (guest == nullptr)
                return;
            ++getGameState().currentTicks;
            guest->update();
        }
    }

    static Guest* CreateLargePocketArrival()
    {
        MapInit({ 80, 80 }, Drawing::Colour::brightGreen);
        getGameState().peepSpawns.clear();
        getGameState().park.entrances.clear();
        for (int y = 10; y < 75; ++y)
        {
            for (int x = 10; x < 75; ++x)
            {
                const CoordsXYZ loc{ x * 32, y * 32, 112 };
                MapGetSurfaceElementAt(loc)->setOwnership(kUnowned);
                auto* path = TileElementInsert<PathElement>(loc, 0);
                path->setClearanceZ(128);
                path->setEdges((x > 10 || y == 10 ? 1 : 0) | (y < 74 ? 2 : 0) | (x < 74 ? 4 : 0) | (y > 10 ? 8 : 0));
            }
        }
        const CoordsXYZ boundary{ 288, 320, 112 };
        MapGetSurfaceElementAt(boundary)->setOwnership({ OwnershipFlag::landOwned });
        TileElementInsert<PathElement>(boundary, 0)->setEdges(4);
        auto* guest = CreateArrival();
        guest->nextLoc = { 320, 320, 112 };
        guest->moveTo(guest->nextLoc.toTileCentre());
        return guest;
    }

    static void AddJunction()
    {
        MapGetFootpathElement({ 256, 160, 112 })->setEdges(7);
        auto* path = TileElementInsert<PathElement>({ 256, 192, 112 }, 0);
        ASSERT_NE(path, nullptr);
        path->setClearanceZ(128);
        path->setEdges(8);
    }
};

TEST_F(EnclosedArrivalTest, LeavesThroughRelocatedGateWithoutAdmission)
{
    AddExit(10);
    auto* guest = CreateArrival();
    const auto id = guest->id;
    TickGuest(id, 40);
    ASSERT_EQ(guest->state, PeepState::leavingPark);
    EXPECT_TRUE(guest->outsideOfPark);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);

    // Simulate an exit already selected at a junction before it was relocated.
    guest->peepFlags.set(PeepFlag::parkEntranceChosen);
    guest->pathfindGoal = { 10, 5, 14, 0 };

    TileElementRemove(reinterpret_cast<TileElement*>(MapGetParkEntranceElementAt({ 320, 160, 112 }, false)));
    getGameState().park.entrances.clear();
    AddExit(3);
    PathFinding::GuestPathFindParkEntranceLeaving(*guest, 5);
    EXPECT_EQ(guest->pathfindGoal.x, 3);
    EXPECT_EQ(guest->pathfindGoal.y, 5);
    TickGuest(id, 2000);
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 10u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 10u);
}

TEST_F(EnclosedArrivalTest, WandersUntilExitIsBuilt)
{
    // Close the outer end while there is no gate.
    MapGetFootpathElement({ 96, 160, 112 })->setEdges(4);
    auto* guest = CreateArrival();
    const auto id = guest->id;
    int minX = guest->x;
    int maxX = guest->x;
    for (int i = 0; i < 1000; ++i)
    {
        TickGuest(id, 1);
        ASSERT_NE(getGameState().entities.getEntity<Guest>(id), nullptr);
        minX = std::min(minX, static_cast<int>(guest->x));
        maxX = std::max(maxX, static_cast<int>(guest->x));
        ASSERT_TRUE(guest->outsideOfPark);
    }
    EXPECT_GT(maxX - minX, 64);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 10u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
    AddExit(3);
    TickGuest(id, 2000);
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 10u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 10u);
}

TEST_F(EnclosedArrivalTest, OutsideArrivalStaysOffOwnedApproach)
{
    auto* guest = CreateArrival();
    guest->moveTo({ 80, 176, 112 });
    guest->nextLoc = { 64, 160, 112 };
    guest->peepDirection = 2;
    guest->setDestination({ 112, 176 }, 2);
    const auto id = guest->id;
    for (int i = 0; i < 2000; ++i)
    {
        guest = getGameState().entities.getEntity<Guest>(id);
        if (guest == nullptr)
            break;
        EXPECT_TRUE(guest->outsideOfPark);
        EXPECT_FALSE(MapIsLocationOwned(guest->nextLoc));
        guest->update();
    }
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 10u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 10u);
}

TEST_F(EnclosedArrivalTest, ArrivalCanEnterGateAtOwnershipBoundary)
{
    AddExit(3);
    auto* guest = CreateArrival();
    guest->moveTo({ 80, 176, 112 });
    guest->nextLoc = { 64, 160, 112 };
    guest->peepDirection = 2;
    guest->setDestination({ 112, 176 }, 2);
    for (int i = 0; i < 200 && guest->outsideOfPark; ++i)
        guest->update();
    EXPECT_FALSE(guest->outsideOfPark);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 11u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 11u);
    guest->remove();
}

TEST_F(EnclosedArrivalTest, GateWithoutOwnedInsidePathDoesNotAdmitGuests)
{
    AddExit(3);
    MapGetSurfaceElementAt(CoordsXY{ 128, 160 })->setOwnership(kUnowned);
    auto* guest = CreateArrival();
    guest->moveTo({ 80, 176, 112 });
    guest->nextLoc = { 64, 160, 112 };
    guest->peepDirection = 2;
    guest->setDestination({ 112, 176 }, 2);
    const auto id = guest->id;
    TickGuest(id, 2000);
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 10u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 10u);
}

TEST_F(EnclosedArrivalTest, ArrivalWithUnownedRouteToSpawnIsNotTreatedAsTrapped)
{
    // The blocked route to the right must not grant permission to enter owned paths.
    for (int x = 3; x <= 8; ++x)
        MapGetSurfaceElementAt(CoordsXY{ x * 32, 160 })->setOwnership(kUnowned);
    auto* guest = CreateArrival();
    guest->peepDirection = 2;
    guest->setDestination({ 304, 176 }, 2);
    for (int i = 0; i < 30; ++i)
    {
        guest->performNextAction();
        EXPECT_EQ(guest->state, PeepState::enteringPark);
        EXPECT_FALSE(MapIsLocationOwned(guest->nextLoc));
    }
    guest->remove();
}

TEST_F(EnclosedArrivalTest, AdmittedGuestLeavingStillDecrementsCount)
{
    AddExit(3);
    auto* guest = CreateArrival();
    DecrementGuestsHeadingForPark();
    guest->outsideOfPark = false;
    guest->state = PeepState::walking;
    guest->peepFlags.set(PeepFlag::leavingPark);
    const auto id = guest->id;
    TickGuest(id, 2000);
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 9u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
}

TEST_F(EnclosedArrivalTest, ArrivalEscapesUnownedPocketBetweenOwnedPaths)
{
    AddExit(3);
    for (int x = 7; x <= 9; ++x)
        MapGetSurfaceElementAt(CoordsXY{ x * 32, 160 })->setOwnership(kUnowned);
    auto* guest = CreateArrival();
    const auto id = guest->id;
    TickGuest(id, 2000);
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 10u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 10u);
}

TEST_F(EnclosedArrivalTest, DepartedGuestCrossesOwnedPathBeyondExit)
{
    AddExit(7);
    for (int x = 3; x < 7; ++x)
        MapGetSurfaceElementAt(CoordsXY{ x * 32, 160 })->setOwnership(kUnowned);
    MapGetSurfaceElementAt(CoordsXY{ 64, 160 })->setOwnership({ OwnershipFlag::landOwned });
    auto* guest = CreateArrival();
    DecrementGuestsHeadingForPark();
    guest->outsideOfPark = false;
    guest->state = PeepState::walking;
    guest->peepFlags.set(PeepFlag::leavingPark);
    const auto id = guest->id;
    TickGuest(id, 2000);
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 9u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 10u);
}

TEST_F(EnclosedArrivalTest, SeekingGuestKeepsGateTargetOnUnownedJunction)
{
    AddExit(3);
    AddJunction();
    auto* guest = CreateArrival();
    TickGuest(guest->id, 1);
    // Complete the transition before changing the ownership underneath the guest again.
    guest->stepProgress = 255;
    TickGuest(guest->id, 1);
    ASSERT_EQ(guest->var37, kPeepLeavingParkSeekEntrance);
    MapGetSurfaceElementAt(CoordsXY{ 256, 160 })->setOwnership(kUnowned);
    PathFinding::CalculateNextDestination(*guest);
    EXPECT_EQ(guest->pathfindGoal.x, 3);
    EXPECT_EQ(guest->pathfindGoal.y, 5);
    guest->remove();
}

TEST_F(EnclosedArrivalTest, DepartedGuestKeepsSpawnTargetOnOwnedJunction)
{
    AddExit(3);
    AddJunction();
    auto* guest = CreateArrival();
    DecrementGuestsHeadingForPark();
    guest->state = PeepState::leavingPark;
    guest->var37 = 1;
    PathFinding::CalculateNextDestination(*guest);
    EXPECT_EQ(guest->pathfindGoal.x, 1);
    EXPECT_EQ(guest->pathfindGoal.y, 5);
    guest->remove();
}

TEST_F(EnclosedArrivalTest, LegacyDepartingGuestCanFindSpawnThroughGate)
{
    AddExit(3);
    AddJunction();
    auto* guest = CreateArrival();
    DecrementGuestsHeadingForPark();
    guest->state = PeepState::leavingPark;
    guest->var37 = 1;
    const auto id = guest->id;
    // Old saves can contain unadmitted guests already heading home from inside the park.
    // Their map-exit target is beyond a gate, rather than on the gate itself.
    EXPECT_EQ(PathFinding::ChooseDirection({ 8, 5, 14 }, { 1, 5, 14 }, *guest, true, RideId::GetNull()), 0);
    TickGuest(id, 2000);
    EXPECT_EQ(getGameState().entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(getGameState().park.numGuestsInPark, 10u);
    EXPECT_EQ(getGameState().park.numGuestsHeadingForPark, 0u);
    EXPECT_EQ(getGameState().park.totalAdmissions, 10u);
}

TEST_F(EnclosedArrivalTest, DepartingGuestCannotRouteInwardThroughGate)
{
    AddExit(3);
    AddJunction();
    auto* guest = CreateArrival();
    DecrementGuestsHeadingForPark();
    guest->state = PeepState::leavingPark;
    guest->var37 = 1;
    MapGetParkEntranceElementAt({ 96, 160, 112 }, false)->setDirection(0);
    EXPECT_EQ(PathFinding::ChooseDirection({ 8, 5, 14 }, { 1, 5, 14 }, *guest, true, RideId::GetNull()), kInvalidDirection);
    guest->remove();
}

TEST_F(EnclosedArrivalTest, DepartingGuestPrefersReachableSpawnOverAnotherGate)
{
    AddExit(3);
    AddJunction();
    auto* guest = CreateArrival();
    DecrementGuestsHeadingForPark();
    guest->state = PeepState::leavingPark;
    guest->var37 = 1;
    EXPECT_EQ(PathFinding::ChooseDirection({ 8, 5, 14 }, { 10, 5, 14 }, *guest, true, RideId::GetNull()), 2);
    guest->remove();
}

TEST_F(EnclosedArrivalTest, LargeUnownedPocketSearchEventuallyCompletes)
{
    auto* guest = CreateLargePocketArrival();
    bool trapped = false;
    for (int update = 0; update < 100 && !trapped; ++update)
    {
        ++getGameState().currentTicks;
        trapped = PathFinding::IsArrivalTrappedByOwnedPaths(*guest);
    }
    EXPECT_TRUE(trapped);
    guest->remove();
}

TEST_F(EnclosedArrivalTest, DownhillGateIsAnEscapeFromAnUnownedPocket)
{
    AddExit(7);
    MapGetSurfaceElementAt(CoordsXY{ 192, 160 })->setOwnership(kUnowned);
    MapGetFootpathElement({ 192, 160, 112 })->setEdges(5);
    auto* inside = MapGetFootpathElement({ 256, 160, 112 });
    inside->setBaseZ(96);
    inside->setSloped(true);
    inside->setSlopeDirection(0);
    auto* guest = CreateArrival();
    guest->nextLoc = { 192, 160, 112 };
    guest->moveTo(guest->nextLoc.toTileCentre());
    EXPECT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guest));
    guest->remove();
}

TEST_F(EnclosedArrivalTest, LargeSearchProgressSurvivesSaveLoad)
{
    auto* guest = CreateLargePocketArrival();
    const auto id = guest->id;
    auto& gameState = getGameState();
    ++gameState.currentTicks;
    ASSERT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guest));
    ASSERT_TRUE(gameState.arrivalPathSearches.contains(id));
    const auto before = gameState.arrivalPathSearches.at(id);
    ASSERT_GT(before.processed, 0u);
    ASSERT_LT(before.processed, before.pending.size());

    MemoryStream stream;
    ParkFileExporter exporter;
    exporter.Export(gameState, stream, kParkFileSaveCompressionLevel);
    const auto complete = [&]() {
        for (int updates = 1; updates <= 100; ++updates)
        {
            ++gameState.currentTicks;
            if (PathFinding::IsArrivalTrappedByOwnedPaths(*gameState.entities.getEntity<Guest>(id)))
                return updates;
        }
        return -1;
    };
    const auto updatesBeforeReload = complete();
    ASSERT_GT(updatesBeforeReload, 0);
    stream.SetPosition(0);
    auto importer = ParkImporter::CreateParkFile(GetContext()->GetObjectRepository());
    importer->LoadFromStream(&stream, false);
    importer->Import(gameState);
    ASSERT_TRUE(gameState.arrivalPathSearches.contains(id));
    const auto& after = gameState.arrivalPathSearches.at(id);
    EXPECT_EQ(after.processed, before.processed);
    EXPECT_EQ(after.pending, before.pending);
    EXPECT_EQ(after.visited, before.visited);
    EXPECT_EQ(after.lastUpdateTick, before.lastUpdateTick);
    EXPECT_EQ(complete(), updatesBeforeReload);
    gameState.entities.getEntity<Guest>(id)->remove();
}

TEST_F(EnclosedArrivalTest, LandEditRestartsPendingSearch)
{
    auto* guest = CreateLargePocketArrival();
    auto& gameState = getGameState();
    gameState.peepSpawns.push_back({ 304, 336, 112, 0 });
    ++gameState.currentTicks;
    ASSERT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guest));
    ASSERT_TRUE(gameState.arrivalPathSearches.contains(guest->id));
    gameState.cheats.sandboxMode = true;
    GameActions::LandSetRightsAction action(CoordsXY{ 288, 320 }, GameActions::LandSetRightSetting::unownLand);
    action.SetFlags({ GameActions::CommandFlag::networked });
    ASSERT_EQ(GameActions::Execute(&action, gameState).error, GameActions::Status::ok);
    EXPECT_TRUE(gameState.arrivalPathSearches.empty());
    ++gameState.currentTicks;
    EXPECT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guest));
    EXPECT_FALSE(gameState.arrivalPathSearches.contains(guest->id));
    guest->remove();
    gameState.cheats.sandboxMode = false;
}

TEST_F(EnclosedArrivalTest, GhostPathRemovalPreservesPendingSearch)
{
    auto* guest = CreateLargePocketArrival();
    auto& gameState = getGameState();
    ++gameState.currentTicks;
    ASSERT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guest));
    ASSERT_TRUE(gameState.arrivalPathSearches.contains(guest->id));
    const auto before = gameState.arrivalPathSearches.at(guest->id);
    auto* ghost = TileElementInsert<PathElement>({ 160, 160, 112 }, 0);
    ghost->setGhost(true);
    gameState.cheats.sandboxMode = true;
    GameActions::FootpathRemoveAction action({ 160, 160, 112 });
    action.SetFlags({ GameActions::CommandFlag::ghost, GameActions::CommandFlag::networked });
    ASSERT_EQ(GameActions::Execute(&action, gameState).error, GameActions::Status::ok);
    EXPECT_EQ(MapGetFootpathElement({ 160, 160, 112 }), nullptr);
    ASSERT_TRUE(gameState.arrivalPathSearches.contains(guest->id));
    EXPECT_EQ(gameState.arrivalPathSearches.at(guest->id).pending, before.pending);
    EXPECT_EQ(gameState.arrivalPathSearches.at(guest->id).processed, before.processed);
    guest->remove();
    EXPECT_TRUE(gameState.arrivalPathSearches.empty());
    gameState.cheats.sandboxMode = false;
}

TEST_F(EnclosedArrivalTest, UnrecordedControlsPreservePendingSearch)
{
    auto* guest = CreateLargePocketArrival();
    auto& gameState = getGameState();
    ++gameState.currentTicks;
    ASSERT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guest));
    const auto before = gameState.arrivalPathSearches.at(guest->id);
    const auto originalSpeed = gGameSpeed;
    const auto originalPaused = gGamePaused;
    GameActions::PauseToggleAction pause;
    pause.SetFlags({ GameActions::CommandFlag::networked });
    EXPECT_EQ(GameActions::Execute(&pause, gameState).error, GameActions::Status::ok);
    GameActions::GameSetSpeedAction speed(2);
    speed.SetFlags({ GameActions::CommandFlag::networked });
    EXPECT_EQ(GameActions::Execute(&speed, gameState).error, GameActions::Status::ok);
    gGameSpeed = originalSpeed;
    gGamePaused = originalPaused;
    ASSERT_TRUE(gameState.arrivalPathSearches.contains(guest->id));
    EXPECT_EQ(gameState.arrivalPathSearches.at(guest->id).pending, before.pending);
    EXPECT_EQ(gameState.arrivalPathSearches.at(guest->id).processed, before.processed);
    guest->remove();
}

TEST_F(EnclosedArrivalTest, RejectedGuestsReleaseSearchSlots)
{
    auto* first = CreateLargePocketArrival();
    std::vector<Guest*> guests{ first };
    for (int i = 0; i < 4; ++i)
    {
        auto* guest = CreateArrival();
        guest->nextLoc = first->nextLoc;
        guest->moveTo(first->getLocation());
        guests.push_back(guest);
    }
    auto& gameState = getGameState();
    ++gameState.currentTicks;
    for (size_t i = 0; i < 4; ++i)
        ASSERT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guests[i]));
    ASSERT_EQ(gameState.arrivalPathSearches.size(), 4u);
    for (size_t i = 0; i < 4; ++i)
    {
        guests[i]->state = PeepState::leavingPark;
        guests[i]->var37 = 1;
    }
    ++gameState.currentTicks;
    EXPECT_FALSE(PathFinding::IsArrivalTrappedByOwnedPaths(*guests.back()));
    EXPECT_EQ(gameState.arrivalPathSearches.size(), 1u);
    EXPECT_TRUE(gameState.arrivalPathSearches.contains(guests.back()->id));
    for (auto* guest : guests)
        guest->remove();
}

TEST_F(EnclosedArrivalTest, SharedInsidePathCheckHandlesSlopesAndInvalidConnections)
{
    const CoordsXYZD entrance{ 224, 160, 112, 2 };
    auto* path = MapGetFootpathElement({ 256, 160, 112 });
    ASSERT_TRUE(ParkEntranceHasValidInsidePath(entrance));
    path->setSloped(true);
    path->setSlopeDirection(2);
    EXPECT_TRUE(ParkEntranceHasValidInsidePath(entrance));
    path->setBaseZ(96);
    path->setSlopeDirection(0);
    EXPECT_TRUE(ParkEntranceHasValidInsidePath(entrance));
    path->setSlopeDirection(1);
    EXPECT_FALSE(ParkEntranceHasValidInsidePath(entrance));
    path->setSlopeDirection(0);
    path->setBaseZ(80);
    EXPECT_FALSE(ParkEntranceHasValidInsidePath(entrance));
    path->setBaseZ(96);
    path->setIsQueue(true);
    EXPECT_FALSE(ParkEntranceHasValidInsidePath(entrance));
    path->setIsQueue(false);
    path->setGhost(true);
    EXPECT_FALSE(ParkEntranceHasValidInsidePath(entrance));
    path->setGhost(false);
    MapGetSurfaceElementAt(CoordsXY{ 256, 160 })->setOwnership(kUnowned);
    EXPECT_FALSE(ParkEntranceHasValidInsidePath(entrance));
}

TEST_F(EnclosedArrivalTest, ConcurrentSearchesStayBoundedAndAllComplete)
{
    auto* first = CreateLargePocketArrival();
    std::vector<EntityId> guests{ first->id };
    for (int i = 0; i < 7; ++i)
    {
        auto* guest = CreateArrival();
        guest->nextLoc = first->nextLoc;
        guest->moveTo(first->getLocation());
        guests.push_back(guest->id);
    }
    auto& gameState = getGameState();
    for (int update = 0; update < 100 && !guests.empty(); ++update)
    {
        ++gameState.currentTicks;
        for (auto it = guests.begin(); it != guests.end();)
        {
            auto* guest = gameState.entities.getEntity<Guest>(*it);
            if (PathFinding::IsArrivalTrappedByOwnedPaths(*guest))
            {
                guest->remove();
                it = guests.erase(it);
            }
            else
            {
                ++it;
            }
            ASSERT_LE(gameState.arrivalPathSearches.size(), 4u);
            ASSERT_LE(gameState.arrivalPathSearchWork, 1024u);
        }
    }
    EXPECT_TRUE(guests.empty());
}

TEST_F(EnclosedArrivalTest, GuestEscapesLargePocketThroughGate)
{
    auto* guest = CreateLargePocketArrival();
    const auto id = guest->id;
    auto& gameState = getGameState();
    MapGetFootpathElement({ 288, 320, 112 })->setEdges(5);
    for (int x = 1; x < 8; ++x)
    {
        auto* path = TileElementInsert<PathElement>({ x * 32, 320, 112 }, 0);
        path->setClearanceZ(128);
        path->setEdges(5);
    }
    auto* entrance = TileElementInsert<EntranceElement>({ 256, 320, 112 }, 0);
    entrance->setEntranceType(EntranceType::parkEntrance);
    entrance->setSequenceIndex(ParkEntranceSequence::centre);
    entrance->setDirection(2);
    gameState.park.entrances.push_back({ 256, 320, 112, 2 });
    gameState.peepSpawns.push_back({ 32, 320, 112, 0 });
    guest->setDestination({ 304, 336 }, 2);
    TickGuest(id, 5000);
    EXPECT_EQ(gameState.entities.getEntity<Guest>(id), nullptr);
    EXPECT_EQ(gameState.park.numGuestsInPark, 10u);
    EXPECT_EQ(gameState.park.totalAdmissions, 10u);
    EXPECT_EQ(gameState.park.numGuestsHeadingForPark, 0u);
    EXPECT_TRUE(gameState.arrivalPathSearches.empty());
}

struct SimplePathfindingScenario
{
    const char* name;
    TileCoordsXYZ start;
    uint32_t steps;

    SimplePathfindingScenario(const char* _name, const TileCoordsXYZ& _start, int _steps)
        : name(_name)
        , start(_start)
        , steps(_steps)
    {
    }

    static std::string ToName(const testing::TestParamInfo<SimplePathfindingScenario>& param_info)
    {
        return param_info.param.name;
    }
};

class SimplePathfindingTest : public PathfindingTestBase, public testing::WithParamInterface<SimplePathfindingScenario>
{
};

TEST_P(SimplePathfindingTest, CanFindPathFromStartToGoal)
{
    const SimplePathfindingScenario& scenario = GetParam();

    ASSERT_PRED_FORMAT1(AssertIsStartPosition, scenario.start);
    TileCoordsXYZ pos = scenario.start;

    auto ride = FindRideByName(scenario.name);
    ASSERT_NE(ride, nullptr);

    auto entrancePos = ride->getStation().entrance;
    TileCoordsXYZ goal = TileCoordsXYZ(
        entrancePos.x - TileDirectionDelta[entrancePos.direction].x,
        entrancePos.y - TileDirectionDelta[entrancePos.direction].y, entrancePos.z);

    const auto succeeded = FindPath(&pos, goal, scenario.steps, ride->id) ? testing::AssertionSuccess()
                                                                          : testing::AssertionFailure()
            << "Failed to find path from " << scenario.start << " to " << goal << " in " << scenario.steps << " steps; reached "
            << pos << " before giving up.";

    EXPECT_TRUE(succeeded);
}

INSTANTIATE_TEST_SUITE_P(
    ForScenario, SimplePathfindingTest,
    ::testing::Values(
        SimplePathfindingScenario("StraightFlat", { 19, 15, 14 }, 24), SimplePathfindingScenario("SBend", { 15, 12, 14 }, 87),
        SimplePathfindingScenario("UBend", { 17, 9, 14 }, 87), SimplePathfindingScenario("CBend", { 14, 5, 14 }, 164),
        SimplePathfindingScenario("TwoEqualRoutes", { 9, 13, 14 }, 89),
        SimplePathfindingScenario("TwoUnequalRoutes", { 3, 13, 14 }, 89),
        SimplePathfindingScenario("StraightUpBridge", { 12, 15, 14 }, 24),
        SimplePathfindingScenario("StraightUpSlope", { 14, 15, 14 }, 24),
        SimplePathfindingScenario("SelfCrossingPath", { 6, 5, 14 }, 211)),
    SimplePathfindingScenario::ToName);

class ImpossiblePathfindingTest : public PathfindingTestBase, public testing::WithParamInterface<SimplePathfindingScenario>
{
};

TEST_P(ImpossiblePathfindingTest, CannotFindPathFromStartToGoal)
{
    const SimplePathfindingScenario& scenario = GetParam();
    TileCoordsXYZ pos = scenario.start;
    ASSERT_PRED_FORMAT1(AssertIsStartPosition, scenario.start);

    auto ride = FindRideByName(scenario.name);
    ASSERT_NE(ride, nullptr);

    auto entrancePos = ride->getStation().entrance;
    TileCoordsXYZ goal = TileCoordsXYZ(
        entrancePos.x + TileDirectionDelta[entrancePos.direction].x,
        entrancePos.y + TileDirectionDelta[entrancePos.direction].y, entrancePos.z);

    EXPECT_FALSE(FindPath(&pos, goal, 10000, ride->id));
}

INSTANTIATE_TEST_SUITE_P(
    ForScenario, ImpossiblePathfindingTest,
    ::testing::Values(
        SimplePathfindingScenario("PathWithGap", { 1, 6, 14 }, 10000),
        SimplePathfindingScenario("PathWithFences", { 11, 6, 14 }, 10000),
        SimplePathfindingScenario("PathWithCliff", { 7, 17, 14 }, 10000)),
    SimplePathfindingScenario::ToName);
