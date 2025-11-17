// ============================================================================
// NoitaSeedSearcher (CPU/CUDA) - Program entry and orchestration (main.cu)
// ----------------------------------------------------------------------------
// What this file does (high level):
// - Wires up the platform (CPU/CUDA) implementation and pulls in all modules.
// - Builds spell tables and biome data that the simulation needs.
// - Initializes the configuration (seed ranges, feature toggles, filters, output).
// - Creates device/host buffers sized for the worldgen + search workload.
// - Kicks off the search loop which iterates seeds and collects matching results.
//
// Notes about the architecture:
// - This project supports both CPU and CUDA builds. Macros in defines.h control
//   which back-end is active. The platform headers expose a unified API.
// - Most .cpp files are directly included here (rather than compiled separately).
//   This is intentional: it gives the compiler visibility across translation
//   units, enabling inlining and helping the CUDA build use the same sources.
// - The simulation has 3 broad phases per seed:
//     1) Precheck (cheap predicates to skip obviously-bad seeds).
//     2) Worldgen per biome (generate Wang-tiled maps and overlay masks).
//     3) Spawn scanning + filtering (chests, wands, potions, pixel scenes...).
// - Output can be a count of passing seeds or a buffer with details per seed.
//
// Reading tip:
// - Skim GenerateSpellData() first to see how spell probability tables are
//   precomputed. Then read main() to understand allocation and control flow.
//   After that, see src/worldgen.cpp (map generation) and src/search.cpp
//   (spawn logic) for the core gameplay data emulation.
// ============================================================================

#include "platforms/platform_implementation.h"

#include "platforms/platform_api.h"
using namespace API_INTERNAL;
#include "src/platform_implementation_src.cpp"

//#include "gui/guiMain.h"
#include "include/compute.h"
#include "include/configuration.h"
#include "include/wak.h"

#ifdef DEBUG_ATOMIC_COUNTERS
#include <atomic>
// Global debug counters (only compiled when DEBUG_ATOMIC_COUNTERS is enabled)
std::atomic<uint64_t> globalChestCounter = 0;
std::atomic<uint64_t> globalHeartCounter = 0;
std::atomic<uint64_t> globalItemCounter = 0;
std::atomic<uint64_t> globalWandCounter = 0;
#endif

// Pull in all major modules. These files implement:
// - biome_impl/biome_map: Wang tiles, biome maps and adjacency.
// - cli: tiny command-line parsing to configure the run.
// - compute: worker dispatch, memory arenas, seed iteration loop.
// - filter: post-generation filters over found spawnables.
// - hbwang: Wang tile generation (image-to-tileset conversion + indexing).
// - misc/output/pathfinding/precheck/search/structs/wak/wandgen/worldgen: core logic.
#include "src/biome_impl.cpp"
#include "src/biome_map.cpp"
#include "src/cli.cpp"
#include "src/compute.cpp"
#include "src/filter.cpp"
#include "src/hbwang.cpp"
#include "src/misc.cpp"
#include "src/output.cpp"
#include "src/pathfinding.cpp"
#include "src/precheck.cpp"
#include "src/search.cpp"
#include "src/structs.cpp"
#include "src/wak.cpp"
#include "src/wandgen.cpp"
#include "src/worldgen.cpp"
#define PNG_IMPL
#include "include/pngutils.h"

// Global toggle from CLI to enable/disable achievement gating (off by default)
extern bool g_useUnlockGating;

#include <array>
#include <chrono>
#include <filesystem>
#include <unordered_set>
#include <string>
#include <cstring>

// Persistent structure used to report progress to the outside world
OutputProgressData d;

// -----------------------------
// Unlock flag (achievement) loader and gating helpers
// -----------------------------
namespace {
	static inline bool starts_with_sv(std::string_view s, std::string_view prefix) {
		return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
	}

	static std::unordered_set<std::string>
	LoadUnlockedFlagsFromDir(const std::filesystem::path& flags_dir,
	                         std::string_view expected_prefix = "card_unlocked_") {
		std::unordered_set<std::string> result;
		std::error_code ec;
		if (!std::filesystem::exists(flags_dir, ec) || !std::filesystem::is_directory(flags_dir, ec))
			return result;
		for (const auto& de : std::filesystem::directory_iterator(flags_dir, ec)) {
			if (ec) break;
			if (!de.is_regular_file()) continue;
			const std::string filename = de.path().filename().string();
			if (!starts_with_sv(filename, expected_prefix)) continue;
			std::string base = filename.substr(expected_prefix.size());
			// If extensions might appear, uncomment:
			// base = std::filesystem::path(base).stem().string();
			result.insert(base);
		}
		return result;
	}

static std::unordered_set<std::string> LoadAllUnlockedFlags() {
		std::unordered_set<std::string> out;
		if (!g_useUnlockGating) return out; // disabled => no flags loaded, all spells eligible
		// Candidate locations: project-local and user persistent flags directory
		auto merge = [&out](const std::unordered_set<std::string>& in){ out.insert(in.begin(), in.end()); };
		merge(LoadUnlockedFlagsFromDir(std::filesystem::path("UNLOCK FLAGS")));
		// Use the same pattern as find_wak(): cached path with auto-detect and dialog fallback.
		{
			std::string flags_dir = find_flags_dir();
			if (!flags_dir.empty()) {
				merge(LoadUnlockedFlagsFromDir(std::filesystem::path(flags_dir)));
			}
		}
		return out;
	}

	// Returns true if no requirement (nullptr/empty/"nil"), or required flag exists in unlocked set
static inline bool IsSpellEligibleGivenFlags(const char* required_flag,
                                             const std::unordered_set<std::string>& unlocked_flags) {
		// If gating is disabled, everything is eligible to preserve backward parity
		if (!g_useUnlockGating) return true;
		if (required_flag == nullptr || required_flag[0] == '\0') return true;
		if (std::strcmp(required_flag, "nil") == 0) return true;
		return unlocked_flags.find(required_flag) != unlocked_flags.end();
	}
}

// --------------------------------------------------------------------------
// GenerateSpellData
// --------------------------------------------------------------------------
// Precomputes several lookup tables for spell generation:
// - spellSpawnableInChests/Boxes: quick “is this spell eligible for source X?”
// - allSpellProbs: cumulative distributions per tier (for fast weighted picks)
// - spellProbs_*: per-tier, per-action-type probability ladders
// The tables are copied once to the device memory (UploadToDevice), and then
// referenced by the simulation kernels (CPU/GPU) without per-seed rebuild.
static void GenerateSpellData() {
	// Achievement/flag gating injection point:
	// - If you want to hide spells until unlocked, filter HTables::spells here
	//   before building the tables below. Specifically:
	//   1) When computing spellSpawnableInChests/Boxes, skip gated spells.
	//   2) When building tier ladders (allSpellProbs) and per-type ladders
	//      (spellProbs_Types/Counts/Sums), exclude gated spells and update counts/sums.
	// - Doing it here preserves deterministic RNG mapping everywhere
	//   (GetRandomAction* simply binary-searches these tables). Post-selection
	//   skipping would change RNG consumption and desync flows.
SpellTables tbl; // Local table holder we will fill, then publish to device/global state
	// Load unlocked flags once (merge of known locations). This preserves RNG parity: we will
	// pre-filter spells during table construction rather than post-filtering during selection.
	std::unordered_set<std::string> unlocked_flags = LoadAllUnlockedFlags();
	std::array<bool, SpellCount> spellSpawnableInChests = {};
for (int j = 0; j < SpellCount; j++) { // For integer j starting at 0; j++ while j < SpellCount
		// Gating: skip spells that require a flag we don't have
		if (!IsSpellEligibleGivenFlags(HTables::spells[j].required_flag, unlocked_flags)) continue;
for (int t = 0; t < 11; t++) { // For tier t from 0 to 10 (inclusive)
if (HTables::spells[j].spawn_probabilities[t] > 0 || HTables::spells[j].s == SPELL_SUMMON_PORTAL || // Eligible if positive prob at this tier or special-cased
HTables::spells[j].s == SPELL_SEA_SWAMP) { // (Second special-cased spell)
spellSpawnableInChests[j] = true; // Mark spell j as chest-eligible
break; // Stop scanning other tiers once eligible
			}
		}
	}
tbl.spellSpawnableInChests = // Store device pointer for chest-eligible mask
		(const bool*)UploadToDevice(spellSpawnableInChests.data(), sizeof(spellSpawnableInChests));

std::array<bool, SpellCount> spellSpawnableInBoxes = {}; // Box-eligible mask (utility/modifier spells)
for (int j = 0; j < SpellCount; j++) { // Loop across all spells again
		// Gating: skip spells that require a flag we don't have
		if (!IsSpellEligibleGivenFlags(HTables::spells[j].required_flag, unlocked_flags)) continue;
if (HTables::spells[j].type == MODIFIER || HTables::spells[j].type == UTILITY) { // Only these types can appear in utility boxes
for (int t = 0; t < 11; t++) { // Check all tiers as well
if (HTables::spells[j].spawn_probabilities[t] > 0 || HTables::spells[j].s == SPELL_SUMMON_PORTAL || // Eligible for this tier or special-cased
					HTables::spells[j].s == SPELL_SEA_SWAMP) {
spellSpawnableInBoxes[j] = true; // Mark spell j as utility-box-eligible
					break;
				}
			}
		}
	}
tbl.spellSpawnableInBoxes = // Store device pointer for box-eligible mask
		(const bool*)UploadToDevice(spellSpawnableInBoxes.data(), sizeof(spellSpawnableInBoxes));

	// Build cumulative distributions per tier for weighted sampling
for (int t = 0; t < 11; t++) { // Build per-tier cumulative ladders (weighted random pick support)
std::array<SpellProb, SpellCount> spellProbs_n = {}; // Temporary storage for cumulative entries
int n = 0; // Number of entries actually used (not all spells are present in each tier)
for (int j = 0; j < SpellCount; j++) { // Scan all spells for this tier
			// Gating: exclude locked actions when building tier ladders
			if (!IsSpellEligibleGivenFlags(HTables::spells[j].required_flag, unlocked_flags)) continue;
if (HTables::spells[j].spawn_probabilities[t] > 0) { // If spell j is available at tier t
tbl.spellTierCounts[t]++; // Increment how many spells are in tier t
tbl.spellTierSums[t] += HTables::spells[j].spawn_probabilities[t]; // Grow cumulative sum for tier t
spellProbs_n[n++] = {tbl.spellTierSums[t], HTables::spells[j].s}; // Append {cumulative sum, spell id} entry
			}
		}
tbl.allSpellProbs[t] = (const SpellProb*)UploadToDevice(spellProbs_n.data(), sizeof(SpellProb) * n); // Upload ladder to device memory
	}

	// Build per-action-type distributions per tier
for (int tier = 0; tier < 11; tier++) { // For each tier, also build ladders per action-type
for (int t = 0; t < 8; t++) { // For each action type (0..7)
for (int j = 0; j < SpellCount; j++) { // Count pass: how many spells are in (tier, type)
				// Gating: exclude locked actions so counts reflect only unlocked set.
				if (!IsSpellEligibleGivenFlags(HTables::spells[j].required_flag, unlocked_flags)) continue;
if ((int)HTables::spells[j].type == t && HTables::spells[j].spawn_probabilities[tier] > 0) { // Spell matches this type and tier
tbl.spellProbs_Counts[tier][t]++; // Increment (tier, type) count
				}
			}
		}
for (int t = 0; t < 8; t++) { // Build the cumulative ladder per (tier, type)
std::array<SpellProb, SpellCount> spellProbs_t_n = {}; // Temporary cumulative storage for this (tier, type)
			int n = 0;
if (tbl.spellProbs_Counts[tier][t] > 0) { // Only proceed if bucket is non-empty
double sum = 0; // Local cumulative sum for this (tier, type)
for (int j = 0; j < SpellCount; j++) { // Scan all spells (filter to type/tier)
				// Gating: exclude locked actions when building cumulative ladder.
				if (!IsSpellEligibleGivenFlags(HTables::spells[j].required_flag, unlocked_flags)) continue;
if ((int)HTables::spells[j].type == t && HTables::spells[j].spawn_probabilities[tier] > 0) { // Keep if matches type+tier
sum += HTables::spells[j].spawn_probabilities[tier]; // Accumulate probability into ladder sum
spellProbs_t_n[n++] = {sum, HTables::spells[j].s}; // Append cumulative entry {sum, spell}
					}
				}
tbl.spellProbs_Sums[tier][t] = sum; // Record final sum for this (tier, type)
tbl.spellProbs_Types[tier][t] = // Store device pointer to this (tier, type) ladder
(const SpellProb*)UploadToDevice(spellProbs_t_n.data(), sizeof(SpellProb) * n); // Upload to device memory
			}
		}
	}
HSetSpellData(&tbl); // Publish populated tables so selection functions can binary-search them
}

#if 0
// Developer playground helpers (CPU and CUDA variants) used for experiments
namespace HELPERS
{

#if 0
	constexpr uint64_t MAX_CNT = INT_MAX;
	__device__ int counter = 0;
	__global__ void CountForEach() {
		uint64_t start = blockDim.x * blockIdx.x + threadIdx.x;
		uint64_t stride = gridDim.x * blockDim.x;
		for (uint64_t i = start; i < MAX_CNT; i += stride) {
			Wand w = GetWandWithLevelGivenSeed(i, 10, false);
			if (w.capacity >= 45 && w.alwaysCast.s == SPELL_NONE) {
				printf("%i %f %i\n", i, w.capacity, w.multicast);
				dAtomicAdd(&counter, 1);
			}
		}
	}

	static void HCountForEach() {
		int hCtr;
		CountForEach << <30, 64 >> > ();
		checkCudaErrors(cudaDeviceSynchronize());
		checkCudaErrors(cudaMemcpyFromSymbol(&hCtr, counter, 4));
		printf("%i %f\n", hCtr, (double)hCtr / MAX_CNT);
	}
#else
	constexpr uint64_t MAX_CNT = 2147483647;
	std::atomic<int> counter = 0;
	void CountForEach(int idx) {
		uint64_t start = idx;
		uint64_t stride = std::thread::hardware_concurrency();
		for (uint64_t i = start; i < MAX_CNT; i += stride) {
			uint8_t chest[1000];
			BiomeWangScope sc = {};
			SpawnableConfig sCfg = {};
			int o = 0, scount = 0;
			CheckNormalChestLoot(0, 0, false, SpawnParams{(int)i, sc, sCfg, {chest, 1000}, o, scount});
			
			if (NollaPRNG(i).Random(1, 10000) == 1)
				counter++;
		}
	}

	static void HCountForEach() {
		{
			std::vector<std::jthread> vec;
			for (int i = 0; i < std::thread::hardware_concurrency(); i++)
				vec.emplace_back(CountForEach, i);
		}
		printf("%i %f\n", counter.load(), (double)counter.load() / MAX_CNT);
	}
#endif
}
#endif

// CLI entrypoint defined in src/cli.cpp
void cli_main(int argc, char** argv);

int main(int argc, char** argv) {
	// Set working directory to the executable’s directory so relative asset
	// paths (data/*) resolve correctly regardless of how we were launched.
	std::filesystem::current_path() = argv[0];
	//HELPERS::HCountForEach();
	//return 0;

	// Load WAK data (asset pack) which contains paths to images (Wang tiles,
	// pixel scenes, etc.). This primes host-side tables in HTables.
	read_wak(find_wak().c_str());

	// -----------------------
	// General run parameters
	// -----------------------
	config.generalCfg = {
#ifdef SEEDS_AS_TRIES
		.seedStart = 1,
		.seedEnd = 100,
#else
		.seedStart = 1,
		.seedEnd = INT_MAX - 1,
#endif
		.seedBlockSize = 1,            // seeds per block a worker handles
		.seedBlockOverride = false,    // let the runtime tune block size
		.priority = 0,                 // platform-specific worker priority
	};

	// --------------------------------
	// Cheap prechecks (once per seed)
	// --------------------------------
	// If any enabled precheck fails, we skip worldgen to save time.
	config.precheckCfg = {
		.cart = {false, CART_NONE},            // boiler cart check (disabled)
		.flask = {false, GOLD},                // specific potion check (disabled)
		.wands = {false, SPELL_NONE, SPELL_NONE},
		.rain = {false, MATERIAL_NONE},
		.alchemy = {false, {}, {}},
		.biomes = {false, {}},
		.fungal = {false, {}},
		.perks = {false, {}, {}, {3, 3, 3, 3, 3, 3, 3}},
		.precheckUpwarps = false,
	};

	// --------------------------------------------
	// Spawn scanning config (evaluated per biome)
	// --------------------------------------------
	config.spawnableCfg = {
		.pwCenter = {0, 0},              // parallel worlds center tile (x,y)
		.pwWidth = {0, 0},               // how many parallel worlds to scan around center
		.minHMidx = 0,                   // first holy-mountain index to include
		.maxHMidx = 6,                   // last holy-mountain index to include
		.greedCurse = false,
		.pacifist = false,
		.shopSpells = false,
		.shopWands = false,
		.eyeRooms = false,
		.biomeChests = false,
		.biomePedestals = false,
		.biomeAltars = false,
		.biomePixelSceneIndexing = true, // index spawns inside pixel scenes onto host tables
		.biomePixelSceneSearch = false,  // emit pixel scene results to output (heavy)
		.biomeEnemies = false,
		.hellShops = false,
		.nightmare = false,
		.genPotions = false,             // if true, output includes actual potion materials
		.genSpells = false,              // if true, output includes actual spell ids
		.genWands = false,               // if true, output includes actual wand layouts
	};

	// --------------------------------------
	// Post-pass filters (run after all biomes)
	// --------------------------------------
	config.filterCfg = {
		.aggregate = false,
		.itemFilterCount = 0,
		.itemFilters = {},
		.materialFilterCount = 0,
		.materialFilters = {},
		.spellFilterCount = 0,
		.spellFilters = {},
		.pixelSceneFilterCount = 0,
		.pixelSceneFilters = {},
		.wandStatFilterCount = 0,
		.wandStatFilters = {},
	};

	// ----------------------
	// Output configuration
	// ----------------------
	config.outputCfg = {
		.outputMode = 1,                 // 1 = text append, see PrintOutputBlock
		.outputFile = "output.txt",
		.printInterval = 15.f,           // seconds between progress logs
		.countPassesOnly = false,        // true: just count valid seeds
		.printProgressLog = true,
		.printOutputToConsole = true,
		.printOutputToFile = false,
	};

	// Parse CLI flags (may override above defaults).
	cli_main(argc, argv);

	// Initialize platform (thread pool and/or CUDA device) and fixed tables.
	InitializePlatform();
	if (DEBUG_DISPATCH_RATE_OVERRIDE)
		SetTargetDispatchRate(DEBUG_DISPATCH_RATE_OVERRIDE);
	HSetBiomeData();
	GenerateSpellData();

	// Load the biome map image and instantiate per-biome scopes
	BiomeMapChunks map = load_biome_map("data/biome_impl/biome_map.png", 0);
	int biomeCount = 0;
	int maxMapArea = 0;
	InstantiateBiomes(config.biomeScopes, biomeCount, maxMapArea, map, biome_list);

	// Auto-tune seed block size based on how many biomes we’re scanning
	config.biomeCount = biomeCount;
	if (!config.generalCfg.seedBlockOverride)
		config.generalCfg.seedBlockSize =
			DEBUG_SEED_BLOCK_OVERRIDE ?
				DEBUG_SEED_BLOCK_OVERRIDE :
				(biomeCount ? (WorkerAppetite > 100 ? 1u : 32u) : (WorkerAppetite > 100 ? 256u : 16384u));

	// Size compute buffers. IMAGE_OUTPUT greatly increases output needs.
	config.memSizes = {
		.memoryCap = 40_GB,

#ifdef IMAGE_OUTPUT
		.outputSize = (size_t)maxMapArea * 3 + 512, // RGB per-pixel + header
#else
		.outputSize = (size_t)8192,                 // default text buffer size
#endif
		.mapDataSize = (size_t)maxMapArea * 2,      // per-pixel map data (uint16_t)
		.miscMemSize = (size_t)maxMapArea * 2,
		.visitedMemSize = (size_t)maxMapArea + 512,
		.spawnableMemSize = max((size_t)maxMapArea / 4, 8192u),
	};

	// Scale spawnable memory with the number of parallel worlds scanned
	config.memSizes.spawnableMemSize *= config.spawnableCfg.pwWidth.x * 2 + 1;
	config.memSizes.spawnableMemSize *= config.spawnableCfg.pwWidth.y * 2 + 1;
	config.memSizes.spawnableMemSize *= max(1, biomeCount);

	// Allocate, run the search, free, and shut down the platform
	AllocateComputeMemory();
	SearchMain(d, nullptr);
	FreeComputeMemory();

	DestroyPlatform();

	//SfmlMain();
	return 0;
}
