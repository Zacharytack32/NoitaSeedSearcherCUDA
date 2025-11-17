// ============================================================================
// Search and loot-generation data structures
// ----------------------------------------------------------------------------
// These structs describe search results, spells, wands, and filters used to
// evaluate whether a seed “passes” the user’s criteria.
//
// Reading order recommendation:
// - Spawnable / SpawnableBlock: how output is serialized and parsed.
// - SpellData / SpellTables: probabilities and precomputed lookup tables.
// - Wand / WandData / WandLevel / BiomeWands: wand stats and per-biome tiers.
// - Filter types: how we match items/materials/spells/wand stats.
// ============================================================================
#pragma once
#include "../platforms/platform_implementation.h"
#include "enums.h"
#include <cstdint>
#include <initializer_list>

// Describes a 2–4 material alchemy recipe and its ordering constraints.
struct AlchemyRecipe {
	// Up to four materials participate; missing slots are MATERIAL_NONE
	Material mats[4] = {MATERIAL_NONE, MATERIAL_NONE, MATERIAL_NONE, MATERIAL_NONE};
	// Some alchemy outcomes depend on the order materials are combined.
	AlchemyOrdering ordering;

	// Compares two recipes respecting both set membership and ordering rules.
	_universal bool Equals(AlchemyRecipe reference, AlchemyRecipe test);
};

// One fungal shift (material->material) outcome window and flags.
struct FungalShift {
	// Source category (what can be shifted from)
	ShiftSource from;
	// Destination category (what it becomes)
	ShiftDest to;
	// Whether the source must be in a flask to be eligible
	bool fromFlask;
	// Whether the result is placed in a flask
	bool toFlask;
	// Shift table inclusive range indices (seed-window)
	int minIdx;
	int maxIdx;
	// If non-zero, stores the RNG roll that selected this outcome (debugging)
	int randomRoll = 0;
	_universal FungalShift();
	_universal FungalShift(ShiftSource _from, ShiftDest _to, int _minIdx, int _maxIdx);
};

// Perk properties as per game data (stacking, rarity, pool limits)
struct PerkData {
	bool stackable;
	bool stackable_rare;
	uint8_t stackable_max;
	uint8_t max_in_pool;
	bool not_default;
	uint8_t stackable_how_often_reappears;
};

// Perk selection metadata for a holy mountain row (lottery, slot constraints)
struct PerkInfo {
	Perk p;
	bool lottery;
	int minPosition;
	int maxPosition;
};

// The on-wire representation of a discovered spawnable in the output buffer
#pragma pack(push, 1)
struct Spawnable {
	int x;
	int y;
	// Variant tag that tells the consumer how to interpret the payload
	SpawnableMetadata sType;
	// Number of bytes that follow for this entry (variable-length data)
	int count;
	// For fixed-size payloads, this may directly encode the item id
	Item contents;
};
#pragma pack(pop)

// A view over a contiguous block of Spawnable entries produced for one seed.
struct SpawnableBlock {
	int seed;
	int count;
	Spawnable** spawnables;
};

#pragma pack(push, 1)
// A spell tagged with metadata (e.g., always-cast vs regular deck spell)
struct LabelledSpell {
	SpawnableMetadata d;
	Spell s;
};
#pragma pack(pop)

// Host-side static spell row. Probabilities are per-tier (11 tiers in total).
struct SpellData {
	Spell s;
	ActionType type;
	double spawn_probabilities[11];
	const char* name;               // internal name used by the simulator
	const char* required_flag;      // achievement/unlock flag name; use "nil" or nullptr if none
};

// Cumulative distribution entry used for O(log n) weighted random picks.
struct SpellProb {
	double p;
	Spell s;
};

// Aggregated, device-visible tables built at startup (GenerateSpellData)
struct SpellTables {
	// Quick eligibility masks used by MakeRandomCard/Utility
	const bool* spellSpawnableInChests;
	const bool* spellSpawnableInBoxes;
	// Tier-wide cumulative ladders for weighted sampling among all spells
	const SpellProb* allSpellProbs[11];
	int spellTierCounts[11];
	double spellTierSums[11];
	// Per-tier, per-action-type cumulative ladders (types indexed 0..7)
	const SpellProb* spellProbs_Types[11][8];
	int spellProbs_Counts[11][8];
	double spellProbs_Sums[11][8];
};

// Full wand description used when genWands/genSpells is enabled.
struct Wand {
	uint8_t level;
	bool isBetter;
	bool force_unshuffle;
	bool is_rare;
	float cost;
	float prob_unshuffle;
	float prob_draw_many;
	// Capacity in this code means effective deck capacity (float for precision)
	float capacity;
	uint16_t mana;
	uint16_t regen;
	int16_t delay;
	int16_t reload;
	float speed;
	uint8_t multicast;
	int8_t spread;
	bool shuffle;
	uint8_t spellCount;
	LabelledSpell alwaysCast;
	LabelledSpell spells[67];
};

#pragma pack(push, 1)
// Compact on-wire layout written into the output buffer for each wand
struct WandData {
	float capacity;
	uint16_t mana;
	uint16_t regen;
	int16_t delay;
	int16_t reload;
	float speed;
	uint8_t multicast;
	int8_t spread;
	bool shuffle;
	uint8_t spellCount;
	LabelledSpell alwaysCast;
};
#pragma pack(pop)

// Probability+id of a shop/altar wand for a biome tier
struct WandLevel {
	float prob;
	Item id;
	_universal constexpr WandLevel() : prob(), id() {}
	_universal constexpr WandLevel(float _p, Item _w) : prob(_p), id(_w) {}
};
// Small fixed-size list of wand tiers per biome (6 entries max)
struct BiomeWands {
	int count;
	WandLevel levels[6];

	_universal constexpr BiomeWands() : count(), levels() {}
	_universal constexpr BiomeWands(std::initializer_list<WandLevel> list) : count(list.size()), levels() {
		Assert(list.size() <= 6, "Initializer list size overflow.");
		for (int i = 0; i < list.size(); i++)
			levels[i] = list.begin()[i];
	}
};

// Sprite parameters used to derive base wand stats (deck, reload, spread...)
struct WandSprite {
	int fileNum;
	int8_t grip_x;
	int8_t grip_y;
	int8_t tip_x;
	int8_t tip_y;
	int8_t fire_rate_wait;
	int8_t actions_per_round;
	bool shuffle_deck_when_empty;
	int8_t deck_capacity;
	int8_t spread_degrees;
	int8_t reload_time;
};
// Floating-point workspace version of WandSprite used during stat math.
struct WandSpaceDat {
	float fire_rate_wait;
	float actions_per_round;
	bool shuffle_deck_when_empty;
	float deck_capacity;
	float spread_degrees;
	float reload_time;
};

/*
struct EnemyData
{
	float prob;
	int minCount;
	int maxCount;
	Enemy enemy;
	_universal constexpr EnemyData() : prob(), minCount(), maxCount(), enemy() {}
	_universal constexpr EnemyData(float _p, int _min, int _max, Enemy _e) : prob(_p), minCount(_min), maxCount(_max), enemy(_e) {}
};
struct EnemyList
{
	int count;
	float probSum;
	EnemyData enemies[20];
	_universal constexpr EnemyList() : count(), probSum(), enemies() {}
	_universal constexpr EnemyList(int _c, std::initializer_list<EnemyData> list) : count(_c), probSum(), enemies()
	{
		float pSum = 0;
		for (int i = 0; i < list.size(); i++)
		{
			pSum += list.begin()[i].prob;
			enemies[i] = list.begin()[i];
		}
		probSum = pSum;
	}
};
*/

// Within a single filter, we allow logical OR across up to this many entries
constexpr auto FILTER_OR_COUNT = 5;
// Upper bound for how many filters of a given type we can process per run
constexpr auto TOTAL_FILTER_COUNT = 10;
// Match seeds that contain any of the listed items (with optional duplicates)
struct ItemFilter {
	Item items[FILTER_OR_COUNT];
	int duplicates;

	ItemFilter();
	ItemFilter(std::initializer_list<Item> _items);
	ItemFilter(std::initializer_list<Item> _items, int _dupes);
};
// Match seeds that contain any of the listed materials
struct MaterialFilter {
	Material materials[FILTER_OR_COUNT];
	int duplicates;

	MaterialFilter();
	MaterialFilter(std::initializer_list<Material> _items);
	MaterialFilter(std::initializer_list<Material> _items, int _dupes);
};
// Match seeds that contain certain spells (optionally as always-cast and/or
// per-wand). Duplicates constrain how many occurrences are required.
struct SpellFilter {
	Spell spells[FILTER_OR_COUNT];
	int duplicates;
	bool asAlwaysCast;
	bool perWand;

	SpellFilter();
	SpellFilter(std::initializer_list<Spell> _spells);
	SpellFilter(std::initializer_list<Spell> _spells, int _dupes);
	SpellFilter(std::initializer_list<Spell> _spells, int _dupes, bool _asAlwaysCast);
	SpellFilter(std::initializer_list<Spell> _spells, int _dupes, bool _asAlwaysCast, bool _consecutive);
};
// Match seeds by pixel scenes (and optionally by the materials they include)
struct PixelSceneFilter {
	PixelScene pixelScenes[FILTER_OR_COUNT];
	Material materials[FILTER_OR_COUNT];
	int duplicates;
	bool checkMats;

	PixelSceneFilter();
	PixelSceneFilter(std::initializer_list<PixelScene> _pixelScenes);
	PixelSceneFilter(std::initializer_list<PixelScene> _pixelScenes, int _dupes);
	PixelSceneFilter(std::initializer_list<PixelScene> _pixelScenes, std::initializer_list<Material> _materials);
	PixelSceneFilter(
		std::initializer_list<PixelScene> _pixelScenes, std::initializer_list<Material> _materials, int _dupes);
};
// Match seeds by a wand stat threshold (>, >=, ==, etc.)
struct WandStatFilter {
	WandStat stat;
	int value;
	int comparison;
	int duplicates;
};