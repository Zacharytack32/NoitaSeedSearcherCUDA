#pragma once

// -----------------------------
// Unlock flag (achievement) gating stubs (comments only)
// -----------------------------
// Suggested approach:
// - At startup, read unlocked flags from the filesystem once:
//   F:\\NoitaStuff\\CustomNCF\\Noitagame\\Noitadata\\save01\\persistent\\flags
//   (empty, named files indicate a flag is set).
// - Maintain a fast lookup (unordered_set<string> or bitset keyed by integer ids).
// - Provide helpers like:
//     // Returns true if the named flag is present in the loaded set.
//     // bool IsFlagUnlocked(const char* flag_name);
//
//     // Returns the required flag name for a spell (or nullptr/"nil").
//     // const char* SpellRequiredFlag(Spell s);
//
// - In GenerateSpellData gating hooks, skip any spell whose required flag is not unlocked:
//     // auto flag = SpellRequiredFlag(HTables::spells[j].s);
//     // if (flag && strcmp(flag, "nil") != 0 && !IsFlagUnlocked(flag)) continue;
//
// Notes:
// - Treat nullptr or "nil" as “no requirement”.
// - This preserves RNG parity: ladders are built from already-gated sets.
#include "primitives.h"
#include "search_structs.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

struct GeneralConfig {
	uint32_t seedStart;
	uint32_t seedEnd;
	uint32_t seedBlockSize;
	bool seedBlockOverride;
	int priority;
};
struct MemSizeConfig {
	size_t memoryCap;

	size_t outputSize;
	size_t mapDataSize;
	size_t miscMemSize;
	size_t visitedMemSize;
	size_t spawnableMemSize;

	size_t threadMemTotal;
};

struct StartingCartConfig {
	bool check;
	CartType cart;
};
struct StartingFlaskConfig {
	bool check;
	Material flask;
};
struct StartingWandConfig {
	bool check;
	Spell projectile;
	Spell bomb;
};
struct RainConfig {
	bool check;
	Material rain;
};
struct AlchemyConfig {
	bool check;
	AlchemyRecipe LC;
	AlchemyRecipe AP;
};
struct BiomeModifierConfig {
	bool check;
	BiomeModifier modifiers[9];
};
constexpr int maxFungalShifts = 12;
struct FungalShiftConfig {
	bool check;
	FungalShift shifts[maxFungalShifts];
};
constexpr int maxPerkFilters = 12;
struct PerkConfig {
	bool check;
	PerkInfo perks[maxPerkFilters];
	Perk ignore_these[2];
	uint8_t perksPerMountain[7];
};

struct StaticPrecheckConfig {
	StartingCartConfig cart;
	StartingFlaskConfig flask;
	StartingWandConfig wands;
	RainConfig rain;
	AlchemyConfig alchemy;
	BiomeModifierConfig biomes;
	FungalShiftConfig fungal;
	PerkConfig perks;
	bool precheckUpwarps;
};

struct SpawnableConfig {
	Vec2i pwCenter;
	Vec2i pwWidth;
	int minHMidx;
	int maxHMidx;
	bool greedCurse;

	bool pacifist;
	bool shopSpells;
	bool shopWands;

	bool eyeRooms;

	bool biomeChests;
	bool biomePedestals;
	bool biomeAltars;
	bool biomePixelSceneIndexing;
	bool biomePixelSceneSearch;
	bool biomeEnemies;
	bool hellShops;
	bool nightmare;

	bool genPotions;
	bool genSpells;
	bool genWands;
};

struct FilterConfig {
	bool aggregate;
	int itemFilterCount;
	ItemFilter itemFilters[TOTAL_FILTER_COUNT];
	int materialFilterCount;
	MaterialFilter materialFilters[TOTAL_FILTER_COUNT];
	int spellFilterCount;
	SpellFilter spellFilters[TOTAL_FILTER_COUNT];
	int pixelSceneFilterCount;
	PixelSceneFilter pixelSceneFilters[TOTAL_FILTER_COUNT];
	int wandStatFilterCount;
	WandStatFilter wandStatFilters[TOTAL_FILTER_COUNT];
};

struct OutputConfig {
	// 0=human-readable, 1=code, 2=binary
	int outputMode;
	const char* outputFile;

	float printInterval;
	bool countPassesOnly;
	bool printProgressLog;
	bool printOutputToConsole;
	bool printOutputToFile;
};