// ============================================================================
// Worldgen and biome/pixel-scene data structures
// ----------------------------------------------------------------------------
// These types describe the geometry of the world (biome layout), the Wang
// tilesets used to synthesize maps, and the preprocessed pixel-scene metadata
// that feed the spawn system. They are shared across worldgen and search.
//
// Reading order suggestion:
// - BiomeMap / BiomeSector: global layout and per-biome geometry.
// - WangSpawn / WangTile / WangTileset: tile palette and in-tile spawn hooks.
// - BiomeWangScope / GeneratedBiome: instantiated scope + generated indices.
// - SpawnParams: context passed to spawn callbacks during scanning.
// - PixelScene* structs: scene tables and discovered spawn markers.
// ============================================================================
#pragma once
#include "../platforms/platform_implementation.h"
#include "enums.h"
#include "primitives.h"
#include "search_structs.h"

// High-level biome grid for the entire world; map[y*w+x] is a Biome id
struct BiomeMap {
	int w, h;
	Biome* map;
};
// Geometry for a single biome region in world coordinates and map dims
struct BiomeSector {
	Biome b;

	int worldX;
	int worldY;
	int worldW;
	int worldH;

	uint32_t tiles_w;
	uint32_t tiles_h;
	uint32_t map_w;
	uint32_t map_h;
	uint8_t wang_w;
	uint8_t wang_h;
};

// Tile index encoding used by stbhw_generate_image (low bits index; high bits flags)
typedef int16_t WangTileIndex;
// Index into a biome’s spawn function table (AllSpawnFunctions[b])
typedef int16_t WangFuncIndex;

// A spawn hook embedded in a Wang tile at (x,y) within the tile footprint
struct WangSpawn {
	uint8_t x;
	uint8_t y;
	WangFuncIndex i;
};
// Upper bound of spawn hooks per tile. Keep in sync with tile authoring.
constexpr int _WangTileMaxSpawns = 6;
// One tile variation. colors[] encode edge color ids used by Wang matching.
// spawns[] hold callbacks to run when this tile is placed.
struct WangTile {
	bool should_block;
	char colors[6];
	WangSpawn spawns[_WangTileMaxSpawns];
};

// Full Wang tileset for a biome (horizontal+vertical variants and indices)
struct WangTileset {
	char is_corner;
	int num_vary[2];
	int num_color[6];
	int max_colors;
	int short_side_len;
	int widthH, heightH, widthV, heightV;
	WangTile hTiles[72];
	WangTile vTiles[72];
	uint16_t hIndices[729];
	uint16_t vIndices[729];
	uint8_t* tileData;
	uint32_t tdStride;
	// Returns palette color at a sub-tile position for horizontal tile (tx,ty)
	_universal uint32_t h_tile_at(int tx, int ty, int xoff, int yoff) const;
	// Returns palette color at a sub-tile position for vertical tile (tx,ty)
	_universal uint32_t v_tile_at(int tx, int ty, int xoff, int yoff) const;
};

struct MainPathFill {
	bool active;
	int x1, x2;
};

// Instantiated scope that binds a tileset to a specific biome region and map
struct BiomeWangScope {
	BiomeMap map;
	WangTileset ts;
	BiomeSector bSec;
};

// Result buffer produced by GenerateMap: placed tile indices and function ids
struct GeneratedBiome {
	const BiomeWangScope& scope;
	WangTileIndex* indices;
	WangFuncIndex* funcs;
};

// Context passed to every spawn function; includes output buffer and counters
struct SpawnParams {
	int seed;
	const BiomeWangScope& currentBiome;
	const SpawnableConfig& sCfg;
	MemSpan bytes;
	int& offset;
	int& sCount;
};

// Color palettes used to detect spawn markers in pixel scene PNGs (per biome)
struct BiomeSpawnColors {
	int count;
	uint32_t colors[12];

	constexpr BiomeSpawnColors() = default;
	constexpr BiomeSpawnColors(std::initializer_list<uint32_t> list) : count(list.size()), colors() {
		Assert(list.size() <= 12, "Spawn function size overflow.");
		for (int i = 0; i < list.size(); i++)
			colors[i] = list.begin()[i];
	}
};

// Per-biome function table of spawn callbacks and optional init() hook
struct BiomeSpawnFunctions {
	int count;
	void (*init)(SpawnParams& params);
	void (*funcs[12])(int, int, const SpawnParams&);

	constexpr BiomeSpawnFunctions() = default;
	_compute constexpr BiomeSpawnFunctions(
		void (*_fn)(SpawnParams& params), std::initializer_list<void (*)(int, int, const SpawnParams&)> list)
		: count(list.size()), init(_fn), funcs() {
		Assert(list.size() <= 12, "Spawn function size overflow.");
		for (int i = 0; i < list.size(); i++)
			funcs[i] = list.begin()[i];
	}
};

// One spawn marker inside a pixel scene (i = spawn function index)
struct PixelSceneSpawn {
	int i;
	short x;
	short y;
	constexpr PixelSceneSpawn() = default;
	_universal constexpr PixelSceneSpawn(int _t, short _x, short _y) : i(_t), x(_x), y(_y) {}
};
// One pixel scene option with probability, allowed materials, and markers
struct PixelSceneData {
	PixelScene scene;
	float prob;
	short materialCount;
	Material materials[16];
	short spawnCount;
	PixelSceneSpawn spawns[8];

	constexpr PixelSceneData() = default;
	_universal constexpr PixelSceneData(PixelScene _scene, float _prob)
		: scene(_scene), prob(_prob), materialCount(0), materials(), spawnCount(0), spawns() {}
	_universal constexpr PixelSceneData(
		PixelScene _scene, float _prob, std::initializer_list<Material> _mats)
		: scene(_scene), prob(_prob), materialCount(_mats.size()), materials(), spawnCount(0), spawns() {
		for (int i = 0; i < materialCount; i++)
			materials[i] = _mats.begin()[i];
	}
};
// Weighted list of pixel scenes for a given slot (probabilities sum to probSum)
struct PixelSceneList {
	int count;
	float probSum;
	PixelSceneData scenes[20];
	constexpr PixelSceneList() = default;
	_universal constexpr PixelSceneList(std::initializer_list<PixelSceneData> list)
		: count(list.size()), probSum(), scenes() {
		Assert(list.size() <= 20, "Pixel scene list size overflow.");
		for (int i = 0; i < list.size(); i++) {
			probSum += list.begin()[i].prob;
			scenes[i] = list.begin()[i];
		}
	}
};
// Collection of pixel scene lists available within a biome
struct BiomePixelScenes {
	int count;
	PixelSceneList lists[10];
	constexpr BiomePixelScenes() = default;
	_universal constexpr BiomePixelScenes(std::initializer_list<PixelSceneList> list)
		: count(list.size())
		, lists() {
		Assert(list.size() <= 10, "Biome pixel scenes size overflow.");
		for (int i = 0; i < list.size(); i++) {
			lists[i] = list.begin()[i];
		}
	}
};

// Host-side, runtime-populated color tables (global + per-biome) used when
// preprocessing pixel scenes before uploading to the device
BiomeSpawnColors HostSpawnColors[B_BIOME_COUNT] = {};
BiomePixelScenes HostPixelSceneLists[B_LIQUIDCAVE + 1] = {};

// Device-visible tables (uploaded) used during shop/wand spawn selection
_data BiomeWands AllWandLevels[B_BIOME_COUNT];
_data BiomeSpawnFunctions AllSpawnFunctions[B_BIOME_COUNT];
_data BiomePixelScenes AllPixelSceneLists[B_LIQUIDCAVE + 1];

// Number of spell ids (sentinel; SPELL_CESSATION is one past the last real)
_data constexpr int SpellCount = SPELL_CESSATION;
// Aggregated spell tables (populated in GenerateSpellData at startup)
_data SpellTables spellTables = {};

// Convenience constants for debug rendering and visualization
constexpr uint32_t COLOR_PURPLE = 0x7f007fU;
constexpr uint32_t COLOR_BLACK = 0x000000U;
constexpr uint32_t COLOR_WHITE = 0xffffffU;
constexpr uint32_t COLOR_YELLOW = 0xffff00U;
constexpr uint32_t COLOR_COFFEE = 0xc0ffeeU;
constexpr uint32_t COLOR_HELL_GREEN = 0x8aff80U;

// Overlay for special blocking/erase zones (coalmine). Not raw RGB:
// palette encoding: 1: B=0x42, 2: G=0x42, 3: G>0x10, 0 otherwise
_compute uint8_t* coalmine_overlay;
