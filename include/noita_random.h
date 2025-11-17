#pragma once
#include "../platforms/platform_implementation.h"
#include "primitives.h"
#include <cstdint>

// ============================================================================
// RNGs used by world generation and loot simulation
// ----------------------------------------------------------------------------
// There are two distinct RNGs modeled here:
// 1) WorldgenPRNG
//    - A very lightweight 32-bit RNG used to drive the Wang tile map generator.
//    - Typically advanced a deterministic number of times based on world seed
//      and map width, to reproduce the game’s “shuffle until valid” behavior.
// 2) NollaPRNG
//    - The main Noita RNG used for procedural decisions (loot, shops, hearts,
//      potions, pixel scenes, etc.).
//    - It supports seeding from 2D coordinates (SetRandomSeed/SetRandomSeedInt)
//      which mirrors in-game usage: RNG seed = f(world_seed, x, y).
//    - Provides helpers for ranges, distributions, and “procedural” sampling
//      that reads a float in [a, b] based on a coordinate-derived seed.
//
// Important: We do NOT change the math of these RNGs. Behavior must match the
// game exactly for the simulator to be predictive. We only add comments.
// ============================================================================

// ----------------
// WorldgenPRNG API
// ----------------
class WorldgenPRNG {
public:
	uint32_t Seed;                     // current RNG state (32-bit)
	_universal WorldgenPRNG(uint32_t seed);
	_universal uint32_t NextU();       // returns next 32-bit unsigned, advances state
	_universal void Next();            // advance state and discard output
};

// -------------
// NollaPRNG API
// -------------
class NollaPRNG {
public:
	uint32_t world_seed;               // immutable world seed used as base
	int Seed;                          // current RNG state (signed for parity with original code)

	// Construct with world seed. You almost always call SetRandomSeed*(x,y)
	// before using Random*, so decisions depend on position as in-game.
	_universal NollaPRNG(uint32_t worldSeed);

	// Seed from floating/int coordinates. The float version mirrors cases where
	// sub-tile offsets are encoded by adding non-integers (e.g., +509.7, +683.1).
	_universal _noinline void SetRandomSeed(double x, double y);
	_universal _noinline void SetRandomSeedInt(int x, int y);

	// Core PRNG stepping and sampling
	_universal float Next();           // next float in [0,1)
	_universal void Prev();            // step RNG backwards (used in some flows)
	_universal double NextD();         // next double in [0,1)
	_universal int Random(int a, int b);     // inclusive integer in [a,b]
	_universal int RandomD(int a, int b);    // variant with different stepping

	// Coordinate-derived sampling (“procedural”): re-seeds based on (x,y),
	// then returns a float/int in the given range, matching in-game behavior.
	_universal float ProceduralRandomf(double x, double y, float a, float b);
	_universal int ProceduralRandomi(double x, double y, int a, int b);

	// Distribution helpers used to bias random choices (e.g., wand stats).
	_universal _noinline float GetDistribution(float mean, float sharpness, float baseline);
	_universal int RandomDistribution(int min, int max, int mean, float sharpness);
	_universal int RandomDistribution(float min, float max, float mean, float sharpness);
	_universal float RandomDistributionf(float min, float max, float mean, float sharpness);
};

// Convenience helpers used across compute and search modules. These are thin
// wrappers that combine NollaPRNG calls with precomputed probability tables.
_compute float random_next(float min, float max, NollaPRNG& random, Vec2i& rnd);
_compute int random_nexti(float min, float max, NollaPRNG& random, Vec2i& rnd);
_compute int pick_random_from_table_backwards(const float* probs, int length, NollaPRNG& random, Vec2i& rnd);
_compute int pick_random_from_table_weighted(const float* probs, float sum, int length, NollaPRNG& random, Vec2i& rnd);

// Picks a world seed deterministically from a timestamp (used in realtime mode)
int pick_world_seed(uint64_t time);
