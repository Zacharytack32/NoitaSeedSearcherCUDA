#include "../include/compute.h"
#include "../include/misc_funcs.h"
#include "../include/noita_random.h"
#include "../include/search_structs.h"
#include "../include/worldgen_structs.h"
#include "../platforms/platform_implementation.h"

#include "../data/potions.h"
#include "../data/temples.h"

#include <cmath>
#include <cstdio>

_compute static void createPotion(double x, double y, Item type, const SpawnParams& params) {
	// If we are NOT generating specific potion materials, just emit the item tag.
	if (!params.sCfg.genPotions)
		writeByte(params.bytes, params.offset, type);
	else {
		// Otherwise, we will append a DATA_MATERIAL tag followed by a 16-bit material id.
		writeByte(params.bytes, params.offset, DATA_MATERIAL);
		
		// NollaPRNG seeded from world seed but additionally position-seeded using fractional offsets.
		// Offsets (-4.5, -4) mirror how Noita perturbs coordinates to get a stable potion identity
		// independent of sub-tile placement artifacts.
		NollaPRNG rnd = NollaPRNG(params.seed);
		rnd.SetRandomSeed(x - 4.5, y - 4);
		switch (type) {
		case POTION_NORMAL:
			// 75%: “magic” materials path; 25%: “standard” materials path
			if (rnd.Random(0, 100) <= 75) {
				// Extremely rare HP regen (<=50 in 100000)
				if (rnd.Random(0, 100000) <= 50)
					writeShort(params.bytes, params.offset, MAGIC_LIQUID_HP_REGENERATION);
				// Rare purifying powder (<=250 in 100000, but only checked if previous missed)
				else if (rnd.Random(200, 100000) <= 250)
					writeShort(params.bytes, params.offset, PURIFYING_POWDER);
				// Rare weakness (<=500 in 100000)
				else if (rnd.Random(250, 100000) <= 500)
					writeShort(params.bytes, params.offset, MAGIC_LIQUID_WEAKNESS);
				// Otherwise pick from magic materials with uniform index over precomputed table
				else
					writeShort(
						params.bytes, params.offset, potionMaterialsMagic[rnd.Random(0, magicMaterialCount - 1)]);
			} else {
				// Standard materials path: pick from standard table
				writeShort(
					params.bytes, params.offset, potionMaterialsStandard[rnd.Random(0, standardMaterialCount - 1)]);
			}
			break;
		case POTION_SECRET:
			// Secret potion picks from its own table
			writeShort(params.bytes, params.offset, potionMaterialsSecret[rnd.Random(0, secretMaterialCount - 1)]);
			break;
		case POTION_RANDOM_MATERIAL:
			// 50/50: either a liquid or a powder (“sand”)
			if (rnd.Random(0, 100) <= 50)
				writeShort(params.bytes, params.offset, potionLiquids[rnd.Random(0, liquidMaterialCount - 1)]);
			else
				writeShort(params.bytes, params.offset, potionSands[rnd.Random(0, sandMaterialCount - 1)]);
			break;
		}
	}
}
_compute static void createWand(double x, double y, Item type, bool addOffset, const SpawnParams& params) {
	// Always emit the high-level item tag first (so consumers can parse even without genWands)
	writeByte(params.bytes, params.offset, type);

	// Map item enum to wand tier/flags: triplets per tier: Tn, TnNS, TnB (better)
	int wandNum = (int)type - (int)WAND_T1;
	int tier = wandNum / 3 + 1;              // 1..10
	bool nonshuffle = wandNum % 3 == 1;      // middle entry of triplet
	bool better = wandNum % 3 == 2;          // last entry of triplet

#ifdef DO_WANDGEN
	// If exact wand payload is not requested, stop here.
	if (type < WAND_T1 || type > WAND_T10NS || !params.sCfg.genWands)
		return;
	else {
		// Some spawns encode offsets before seeding the RNG, matching in-game behavior.
		int rand_x = (int)x;
		int rand_y = (int)y;

		if (addOffset) {
			rand_x += 510;   // matches Noita’s internal seeding offset for chest wands
			rand_y += 683;   // likewise for Y
		}

		// Build a wand deterministically from world seed + position + tier flags.
		Wand w = GetWandWithLevel(params.seed, rand_x, rand_y, tier, nonshuffle, better, params.sCfg.genSpells);
		
		// Emit compact wand payload: tag + fixed 23-byte header + spells (3 bytes each)
		writeByte(params.bytes, params.offset, DATA_WAND); //-1
		params.bytes.is_safe("createWand", "output", params.offset + 22 + w.spellCount * 3);
		cMemcpyU(params.bytes.ptr + params.offset, &w.capacity, 23);
		params.offset += 23;
		cMemcpyU(params.bytes.ptr + params.offset, w.spells, w.spellCount * 3);
		params.offset += w.spellCount * 3;
	}
#endif
}

_compute static Spell MakeRandomCard(NollaPRNG& random) {
	// NOTE: Gating should be pre-applied in GenerateSpellData(). Do not post-filter here,
	// or you will change RNG consumption and drift from parity. If you absolutely must
	// add a safety check, use a table mask that was built with gating:
	// Example (commented):
	//   while (true) {
	//     int itemno = random.Random(0, SpellCount - 1);
	//     if (!spellTables.spellSpawnableInChests[itemno]) continue; // mask already gated
	//     return (Spell)(itemno + 1);
	//   }
	Spell res = SPELL_NONE;
	char valid = 0;
	while (valid == 0) {
		int itemno = random.Random(0, SpellCount - 1);
		if (spellTables.spellSpawnableInChests[itemno]) {
			return (Spell)(itemno + 1);
		}
	}
	return res;
}
_compute static Spell MakeRandomUtility(NollaPRNG& random) {
	// NOTE: Same as MakeRandomCard: gating must be reflected in spellTables during build.
	// Do not attempt to post-filter here unless you re-sample identically; prefer masks:
	//   while (true) {
	//     int itemno = random.Random(0, SpellCount - 1);
	//     if (!spellTables.spellSpawnableInBoxes[itemno]) continue; // mask already gated
	//     return (Spell)(itemno + 1);
	//   }
	Spell res = SPELL_NONE;
	char valid = 0;
	while (valid == 0) {
		int itemno = random.Random(0, SpellCount - 1);
		if (spellTables.spellSpawnableInBoxes[itemno]) {
			return (Spell)(itemno + 1);
		}
	}
	return res;
}

_compute _noinline static void CheckNormalChestLoot(int x, int y, bool hasMimicSign, const SpawnParams& params) {
#ifdef DEBUG_ATOMIC_COUNTERS
	globalChestCounter++;
#endif
	// We emit one Spawnable entry; sCount is a count of entries emitted for this biome/seed
	params.sCount++;
	// Header: position and type
	writeInt(params.bytes, params.offset, x);
	writeInt(params.bytes, params.offset, y);
	writeByte(params.bytes, params.offset, TYPE_CHEST);
	// Reserve space for payload size; we’ll fill it at the end
	int countOffset = params.offset;
	params.offset += 4;

	// Optional mimic sign tag
	if (hasMimicSign)
		writeByte(params.bytes, params.offset, MIMIC_SIGN);

	// Seed RNG using floating offsets that match chest seeding in Noita
	NollaPRNG random = NollaPRNG(params.seed);
	random.SetRandomSeed(roundRNGPos(x) + 509.7, y + 683.1);

	int count = 1; // number of rolls (some outcomes add more rolls)
	while (count > 0) {
		count--;
		int rnd = random.Random(1, 100); // main chest table
		if (rnd <= 7)
			// Bomb item
			writeByte(params.bytes, params.offset, BOMB);
		else if (rnd <= 40) {
			// Gold nuggets branch; amount depends on a secondary roll
			int amount = 5;
			rnd = random.Random(0, 100);
			if (rnd <= 80) {
				amount = 7;
			} else if (rnd <= 95) {
				amount = 10;
			} else {
				amount = 20;
			}

			// Consume some RNG for sparkle or variations; rare paths consume extra
			rnd = random.Random(0, 100);
			if (rnd > 30) {
				random.Next();
				random.Next();
			} else if (rnd > 99) {
				int tamount = random.Random(1, 3);
				for (int i = 0; i < tamount; i++) {
					random.Next();
					random.Next();
				}
				if (random.Random(0, 100) > 50) {
					tamount = random.Random(1, 3);
					for (int i = 0; i < tamount; i++) {
						random.Next();
						random.Next();
					}
				}
				if (random.Random(0, 100) > 80) {
					tamount = random.Random(1, 3);
					for (int i = 0; i < tamount; i++) {
						random.Next();
						random.Next();
					}
				}
			}
			// Append that many gold nugget bytes
			for (int i = 0; i < amount; i++) {
				random.Next();
				random.Next();
			}
			writeByte(params.bytes, params.offset, GOLD_NUGGETS);
		} else if (rnd <= 50) {
			// Potion branch: mostly normal, sometimes powder, sometimes secret/random-mat
			rnd = random.Random(0, 100);
			if (rnd <= 94)
				createPotion(roundRNGPos(x) + 510, y + 683, POTION_NORMAL, params);
			else if (rnd <= 98)
				writeByte(params.bytes, params.offset, POWDER);
			else {
				rnd = random.Random(0, 100);
				if (rnd <= 98)
					createPotion(roundRNGPos(x) + 510, y + 683, POTION_SECRET, params);
				else
					createPotion(roundRNGPos(x) + 510, y + 683, POTION_RANDOM_MATERIAL, params);
			}
		} else if (rnd <= 54) {
			// Spell refresh vs mimic refresh
			rnd = random.Random(0, 100);
			if (rnd <= 98)
				writeByte(params.bytes, params.offset, Item::SPELL_REFRESH);
			else
				writeByte(params.bytes, params.offset, Item::REFRESH_MIMIC);
		} else if (rnd <= 60) {
			// Special items or runestones (127 sentinel chooses a runestone)
			Item opts[8] = {KAMMI, KUU, UKKOSKIVI, PAHA_SILMA, KIUASKIVI, (Item)127, CHAOS_DIE, SHINY_ORB};
			rnd = random.Random(0, 7);
			Item opt = opts[rnd];
			if ((int)opt == 127) {
				Item r_opts[7] = {RUNESTONE_LIGHT, RUNESTONE_FIRE, RUNESTONE_MAGMA, RUNESTONE_WEIGHT,
					RUNESTONE_EMPTINESS, RUNESTONE_EDGES, RUNESTONE_METAL};
				rnd = random.Random(0, 6);
				Item r_opt = r_opts[rnd];
				writeByte(params.bytes, params.offset, r_opt);
			} else {
				writeByte(params.bytes, params.offset, opt);
			}
		} else if (rnd <= 65) {
			// Random spells: choose amount, then either DATA_SPELL + id, or RANDOM_SPELL if not materializing
			int amount = 1;
			int rnd2 = random.Random(0, 100);
			if (rnd2 <= 50)
				amount = 1;
			else if (rnd2 <= 70)
				amount += 1;
			else if (rnd2 <= 80)
				amount += 2;
			else if (rnd2 <= 90)
				amount += 3;
			else
				amount += 4;

			for (int i = 0; i < amount; i++) {
				random.Next();
				Spell s = MakeRandomCard(random);
				if (params.sCfg.genSpells) {
					writeByte(params.bytes, params.offset, DATA_SPELL);
					writeShort(params.bytes, params.offset, s);
				} else {
					writeByte(params.bytes, params.offset, RANDOM_SPELL);
				}
			}
		} else if (rnd <= 84) {
			// Wand branch: weighted toward lower tiers
			rnd = random.Random(0, 100);
			if (rnd <= 25)
				createWand(x, y, WAND_T1, true, params);
			else if (rnd <= 50)
				createWand(x, y, WAND_T1NS, true, params);
			else if (rnd <= 75)
				createWand(x, y, WAND_T2, true, params);
			else if (rnd <= 90)
				createWand(x, y, WAND_T2NS, true, params);
			else if (rnd <= 96)
				createWand(x, y, WAND_T3, true, params);
			else if (rnd <= 98)
				createWand(x, y, WAND_T3NS, true, params);
			else if (rnd <= 99)
				createWand(x, y, WAND_T4, true, params);
			else
				createWand(x, y, WAND_T4NS, true, params);
		} else if (rnd <= 95) {
			// Heart branch: normal > mimic > bigger > full heal
			rnd = random.Random(0, 100);
			if (rnd <= 88)
				writeByte(params.bytes, params.offset, HEART_NORMAL);
			else if (rnd <= 89)
				writeByte(params.bytes, params.offset, HEART_MIMIC);
			else if (rnd <= 99)
				writeByte(params.bytes, params.offset, HEART_BIGGER);
			else
				writeByte(params.bytes, params.offset, FULL_HEAL);
		} else if (rnd <= 98)
			// Transform chest to gold
			writeByte(params.bytes, params.offset, CHEST_TO_GOLD);
		else if (rnd <= 99)
			// “Chain” more rolls (rare)
			count += 2;
		else
			count += 3;
	}
	// Write payload byte count into the reserved header slot
	writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
}
_compute _noinline static void CheckGreatChestLoot(int x, int y, bool hasMimicSign, const SpawnParams& params) {
	// Similar to normal chests but with different odds and high-end outcomes
	params.sCount++;
	writeInt(params.bytes, params.offset, x);
	writeInt(params.bytes, params.offset, y);
	writeByte(params.bytes, params.offset, TYPE_CHEST_GREATER);
	int countOffset = params.offset;
	params.offset += 4;

	if (hasMimicSign)
		writeByte(params.bytes, params.offset, MIMIC_SIGN);

	NollaPRNG random = NollaPRNG(params.seed);
	random.SetRandomSeedInt(roundRNGPos(x), y);

	int count = 1;

	// Ultra-rare path: TRUE_ORB or SAMPO
	if (random.Random(0, 100000) >= 100000) {
		count = 0;
		if (random.Random(0, 1000) == 999)
			writeByte(params.bytes, params.offset, TRUE_ORB);
		else
			writeByte(params.bytes, params.offset, SAMPO);
	}

	while (count != 0) {
		count--;
		int rnd = random.Random(1, 100);

		if (rnd <= 10) {
			// Heavy potion drop: either 2 normals + 1 secret, or 2 secret + 1 random
			rnd = random.Random(0, 100);
			if (rnd <= 30) {
				createPotion(x, y, POTION_NORMAL, params);
				createPotion(x, y, POTION_NORMAL, params);
				createPotion(x, y, POTION_SECRET, params);
			} else {
				createPotion(x, y, POTION_SECRET, params);
				createPotion(x, y, POTION_SECRET, params);
				createPotion(x, y, POTION_RANDOM_MATERIAL, params);
			}
		} else if (rnd <= 15) {
			// Gold rain
			writeByte(params.bytes, params.offset, RAIN_GOLD);
		} else if (rnd <= 18) {
			// Rare stone vs joke item
			rnd = random.Random(1, 30);
			if (rnd == 30)
				writeByte(params.bytes, params.offset, KAKKAKIKKARE);
			else
				writeByte(params.bytes, params.offset, VUOKSIKIVI);
		} else if (rnd <= 39) {
			// Higher-tier wands compared to normal chest branch
			rnd = random.Random(0, 100);
			if (rnd <= 25)
				createWand(x, y, WAND_T4, false, params);
			else if (rnd <= 50)
				createWand(x, y, WAND_T4NS, false, params);
			else if (rnd <= 75)
				createWand(x, y, WAND_T5, false, params);
			else if (rnd <= 90)
				createWand(x, y, WAND_T5NS, false, params);
			else if (rnd <= 96)
				createWand(x, y, WAND_T6, false, params);
			else if (rnd <= 98)
				createWand(x, y, WAND_T6NS, false, params);
			else if (rnd <= 99)
				createWand(x, y, WAND_T6, false, params);
			else
				createWand(x, y, WAND_T10, false, params);
		} else if (rnd <= 60) {
			// Heart vs bigger vs full heal
			rnd = random.Random(0, 100);
			if (rnd <= 89)
				writeByte(params.bytes, params.offset, HEART_NORMAL);
			else if (rnd <= 99)
				writeByte(params.bytes, params.offset, HEART_BIGGER);
			else
				writeByte(params.bytes, params.offset, FULL_HEAL);
		} else if (rnd <= 98)
			count += 2;  // more rolls
		else
			count += 3;
	}
	writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
}
_compute _noinline static void CheckItemPedestalLoot(int x, int y, const SpawnParams& params) {
#ifdef DEBUG_ATOMIC_COUNTERS
	globalItemCounter++;
#endif
	params.sCount++;
	writeInt(params.bytes, params.offset, x);
	writeInt(params.bytes, params.offset, y);
	writeByte(params.bytes, params.offset, TYPE_ITEM_PEDESTAL);
	int countOffset = params.offset;
	params.offset += 4;

	NollaPRNG random = NollaPRNG(params.seed);
	random.SetRandomSeedInt(x, y);
	int rnd = random.Random(1, 1000);
	if (rnd > 995 && y >= 512 * 3) {
		writeByte(params.bytes, params.offset, MIMIC_POTION);
		writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
		return;
	}

	random.SetRandomSeedInt(x + 425, y - 243);

	rnd = random.Random(1, 91);

	if (rnd <= 65)
		createPotion(x, y - 2, POTION_NORMAL, params);
	else if (rnd <= 70)
		writeByte(params.bytes, params.offset, POWDER);
	else if (rnd <= 71)
		writeByte(params.bytes, params.offset, CHAOS_DIE);
	else if (rnd <= 72) {
		uint8_t r_opts[7] = {RUNESTONE_LIGHT, RUNESTONE_FIRE, RUNESTONE_MAGMA, RUNESTONE_WEIGHT, RUNESTONE_EMPTINESS,
			RUNESTONE_EDGES, RUNESTONE_METAL};
		rnd = random.Random(0, 6);
		uint8_t r_opt = r_opts[rnd];
		writeByte(params.bytes, params.offset, r_opt);
	} else if (rnd <= 73)
		writeByte(params.bytes, params.offset, EGG_PURPLE);
	else if (rnd <= 77)
		writeByte(params.bytes, params.offset, EGG_SLIME);
	else if (rnd <= 79)
		writeByte(params.bytes, params.offset, EGG_MONSTER);
	else if (rnd <= 83)
		writeByte(params.bytes, params.offset, KIUASKIVI);
	else if (rnd <= 85)
		writeByte(params.bytes, params.offset, UKKOSKIVI);
	else if (rnd <= 89)
		writeByte(params.bytes, params.offset, BROKEN_WAND);
	else
		writeByte(params.bytes, params.offset, SHINY_ORB);

	writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
}
_compute _noinline static void CheckUtilityBoxLoot(int x, int y, const SpawnParams& params) {
#ifdef DEBUG_ATOMIC_COUNTERS
	globalChestCounter++;
#endif
	params.sCount++;
	writeInt(params.bytes, params.offset, x);
	writeInt(params.bytes, params.offset, y);
	writeByte(params.bytes, params.offset, TYPE_UTILITY_BOX);
	int countOffset = params.offset;
	params.offset += 4;

	NollaPRNG random = NollaPRNG(params.seed);
	random.SetRandomSeed(roundRNGPos(x) + 509.7, y + 683.1);

	int count = 1;
	while (count > 0) {
		count--;
		int rnd = random.Random(1, 100);
		if (rnd <= 2)
			writeByte(params.bytes, params.offset, BOMB);
		else if (rnd <= 5) {
			rnd = random.Random(0, 100);
			if (rnd == 99)
				writeByte(params.bytes, params.offset, Item::REFRESH_MIMIC);
			else
				writeByte(params.bytes, params.offset, Item::SPELL_REFRESH);
		} else if (rnd <= 11) {
			Item opts[8] = {KAMMI, KUU, UKKOSKIVI, PAHA_SILMA, KIUASKIVI, (Item)127, CHAOS_DIE, SHINY_ORB};
			rnd = random.Random(0, 7);
			Item opt = opts[rnd];
			if ((int)opt == 127) {
				Item r_opts[7] = {RUNESTONE_LIGHT, RUNESTONE_FIRE, RUNESTONE_MAGMA, RUNESTONE_WEIGHT,
					RUNESTONE_EMPTINESS, RUNESTONE_EDGES, RUNESTONE_METAL};
				rnd = random.Random(0, 6);
				Item r_opt = r_opts[rnd];
				writeByte(params.bytes, params.offset, r_opt);
			} else {
				writeByte(params.bytes, params.offset, opt);
			}
		} else if (rnd <= 97) {
			int amount = 2;
			int rnd2 = random.Random(0, 100);
			if (rnd2 <= 40)
				amount = 2;
			else if (rnd2 <= 60)
				amount += 1;
			else if (rnd2 <= 77)
				amount += 2;
			else if (rnd2 <= 90)
				amount += 3;
			else
				amount += 4;

			for (int i = 0; i < amount; i++) {
				random.Next();
				Spell s = MakeRandomUtility(random);
				if (params.sCfg.genSpells) {
					writeByte(params.bytes, params.offset, DATA_SPELL);
					writeShort(params.bytes, params.offset, s);
				}
			}
			if (!params.sCfg.genSpells)
				writeByte(params.bytes, params.offset, RANDOM_SPELL);
		} else if (rnd <= 99)
			count += 2;
		else
			count += 3;
	}
	writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
}

_compute void spawnHeart(int x, int y, const SpawnParams& params) {
	NollaPRNG random = NollaPRNG(params.seed);
	float r = random.ProceduralRandomf(x, y, 0, 1);
	float heart_spawn_percent = 0.7f;

	if (r > heart_spawn_percent) {
		if (!params.sCfg.biomePedestals)
			return;
#ifdef DEBUG_ATOMIC_COUNTERS
		globalHeartCounter++;
#endif
		params.sCount++;
		writeInt(params.bytes, params.offset, x);
		writeInt(params.bytes, params.offset, y);
		writeByte(params.bytes, params.offset, TYPE_ITEM_PEDESTAL);
		writeInt(params.bytes, params.offset, 1);
		writeByte(params.bytes, params.offset, HEART_NORMAL);
	} else if (r > 0.3) {
		if (!params.sCfg.biomeChests)
			return;
		random.SetRandomSeed(x + 45, y - 2123);
		int rnd = random.Random(1, 100);
		if (rnd <= 90 || y < 512 * 3) {
			rnd = random.Random(1, 1000);
			bool hasSign = false;
			if (random.Random(1, 300) == 1) {
				hasSign = true;
			}
			if (rnd >= 1000)
				CheckGreatChestLoot(x, y, hasSign, params);
			else
				CheckNormalChestLoot(x, y, hasSign, params);
		} else {
			params.sCount++;
			writeInt(params.bytes, params.offset, x);
			writeInt(params.bytes, params.offset, y);
			writeByte(params.bytes, params.offset, TYPE_CHEST);
			int countOffset = params.offset;
			params.offset += 4;
			int totalBytes = 1;

			rnd = random.Random(1, 100);
			if (random.Random(1, 30) == 1) {
				writeByte(params.bytes, params.offset, MIMIC_SIGN);
				totalBytes++;
			}
			if (rnd <= 95)
				writeByte(params.bytes, params.offset, MIMIC);
			else
				writeByte(params.bytes, params.offset, MIMIC_LEGGY);
			writeInt(params.bytes, countOffset, totalBytes);
		}
	}
}
_compute void spawnChest(int x, int y, const SpawnParams& params) {
	if (!params.sCfg.biomeChests)
		return;
	NollaPRNG random = NollaPRNG(params.seed);
	random.SetRandomSeedInt(x, y);
	int super_chest_spawn_rate = params.sCfg.greedCurse ? 100 : 2000;
	int rnd = random.Random(1, super_chest_spawn_rate);

	if (rnd >= super_chest_spawn_rate - 1)
		CheckGreatChestLoot(x, y, false, params);
	else
		CheckNormalChestLoot(x, y, false, params);
}
_compute void spawnPotion(int x, int y, const SpawnParams& params) {
	if (!params.sCfg.biomePedestals)
		return;
	NollaPRNG random = NollaPRNG(params.seed);
	float rnd = random.ProceduralRandomf(x, y, 0, 1);

	if (rnd > 0.65f)
		CheckItemPedestalLoot(x + 5, y - 4, params);
}
_compute void spawnWand(int x, int y, const SpawnParams& params) {
	// Only generate if biome altar spawns are enabled
	if (!params.sCfg.biomeAltars)
		return;
#ifdef DEBUG_ATOMIC_COUNTERS
	globalWandCounter++;
#endif

	// Procedural RNG seeded from world seed; we then use a coordinate-derived
	// float in [0,1) to select a wand tier according to BiomeWands probabilities.
	NollaPRNG random = NollaPRNG(params.seed);
	BiomeWands wandSet = AllWandLevels[params.currentBiome.bSec.b];
	float sum = 0;
	for (int i = 0; i < wandSet.count; i++)
		sum += wandSet.levels[i].prob; // total probability mass
	// The (-5, 0) offset mirrors in-game noise usage to decorrelate against neighbors
	float r = random.ProceduralRandomf(x - 5, y, 0, 1) * sum;
	for (int i = 0; i < wandSet.count; i++) {
		// Walk the probability ladder; first bucket with r <= prob is the pick
		if (r <= wandSet.levels[i].prob) {
			params.sCount++;
			writeInt(params.bytes, params.offset, x);
			writeInt(params.bytes, params.offset, y + 5); // altar is slightly below marker
			writeByte(params.bytes, params.offset, TYPE_WAND_PEDESTAL);
			int countOffset = params.offset;
			params.offset += 4;
			// Emit wand item and (optionally) full wand payload; addOffset=false here
			createWand(x, y + 5, wandSet.levels[i].id, false, params);
			writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
			return;
		}
		// Otherwise reduce r and continue; equivalent to cumulative bin pick
		r -= wandSet.levels[i].prob;
	}
}

_compute void LoadPixelScene(int x, int y, const PixelSceneList& list, int dim, const SpawnParams& params) {
	// Verify we are still in the same biome tile the scene belongs to.
	// The calculation depends on whether the caller is in an H/V tile; dim 2 means unknown so skip.
	if (dim != 2) {
		Vec2i chunk = GetLocalPos(x + params.currentBiome.ts.short_side_len * 10 * (1 + (dim == 0)),
			y + params.currentBiome.ts.short_side_len * 10 * (1 + (dim == 1)));
		Biome cBiome = params.currentBiome.map.map[mod(chunk.y, params.currentBiome.map.h) * params.currentBiome.map.w +
												   mod(chunk.x, params.currentBiome.map.w)];
		if (cBiome != params.currentBiome.bSec.b)
			return;
	}

	// Randomly pick a scene by probability, and optionally pick a required material variant
	NollaPRNG random = NollaPRNG(params.seed);
	PixelSceneData pickedScene = {};
	Material pickedMat = MATERIAL_NONE;
	if (list.count > 1) {
		float rnd2 = random.ProceduralRandomf(x, y, 0, list.probSum);

		for (int i = 0; i < list.count; i++) {
			if (rnd2 <= list.scenes[i].prob) {
				pickedScene = list.scenes[i];
				break;
			}
			rnd2 -= list.scenes[i].prob;
		}
	} else
		pickedScene = list.scenes[0];

	if (!pickedScene.scene)
		return;

	// If enabled, record the pixel scene into output with an optional material constraint.
	if (params.sCfg.biomePixelSceneSearch) {
		if (pickedScene.materialCount > 0) {
			int idx = (int)rintf(random.ProceduralRandomf(x + 11, y - 21, 0, pickedScene.materialCount - 1));
			pickedMat = pickedScene.materials[idx];
		}

		params.sCount++;
		writeInt(params.bytes, params.offset, x);
		writeInt(params.bytes, params.offset, y);
		writeByte(params.bytes, params.offset, TYPE_PIXEL_SCENE);
		writeInt(params.bytes, params.offset, 5);
		writeByte(params.bytes, params.offset, DATA_PIXEL_SCENE);
		writeShort(params.bytes, params.offset, pickedScene.scene);
		writeShort(params.bytes, params.offset, pickedMat);
	}

	// For each spawn marker encoded in the scene PNG, dispatch the appropriate spawn.
	for (int idx = 0; idx < pickedScene.spawnCount; idx++) {
		PixelSceneSpawn spawn = pickedScene.spawns[idx];
		Vec2i chunk = GetLocalPos(x + spawn.x, y + spawn.y);
		Biome cBiome = params.currentBiome.map.map[mod(chunk.y, params.currentBiome.map.h) * params.currentBiome.map.w +
												   mod(chunk.x, params.currentBiome.map.w)];
		if (cBiome != params.currentBiome.bSec.b)
			continue;

		// Note: we index spawns by function index, not by color here.
		switch (spawn.i) {
		case 0: spawnHeart(x + spawn.x, y + spawn.y, params); break;
		case 1: spawnChest(x + spawn.x, y + spawn.y, params); break;
		case 2: spawnPotion(x + spawn.x, y + spawn.y, params); break;
		case 3: spawnWand(x + spawn.x, y + spawn.y, params); break;
		}
	}
}

/*_compute void SpawnEnemies(int x, int y, EnemyList list, const SpawnParams& params)
{
	NollaPRNG random = NollaPRNG(params.seed);
	float rnd2 = random.ProceduralRandomf(x, y, 0, list.probSum);

	EnemyData pickedEnemy;
	for (int i = 0; i < list.count; i++)
	{
		if (rnd2 <= list.enemies[i].prob)
		{
			pickedEnemy = list.enemies[i];
			break;
		}
		rnd2 -= list.enemies[i].prob;
	}
	if (pickedEnemy.enemy == ENEMY_NONE) return;
	int enemyCount = random.ProceduralRandomi(x + 6 + (pickedEnemy.enemy == ENEMY_HAMIS ? 4 : 0), y + 5, pickedEnemy.minCount, pickedEnemy.maxCount);

	params.sCount++;
	writeInt(params.bytes, params.offset, x);
	writeInt(params.bytes, params.offset, y);
	writeByte(params.bytes, params.offset, TYPE_ENEMY);
	writeInt(params.bytes, params.offset, enemyCount);
	for (int i = 0; i < enemyCount; i++)
		writeByte(params.bytes, params.offset, pickedEnemy.enemy == ENEMY_HAMIS ? WAND_T1 : GOLD_NUGGETS);
	//writeShort(params.bytes, params.offset, pickedScene.scene);
	//writeShort(params.bytes, params.offset, pickedMat);
}*/

/*
	//t10 wands only
	if (floorf(_y / (512 * 4.0f)) <= 7) return;
	if (x < params.mCfg.minX || x > params.mCfg.maxX || y < params.mCfg.minY || y > params.mCfg.maxY) return;

	NollaPRNG random = NollaPRNG(seed);

	//enemy spawns
	//if (random.ProceduralRandomf(_x, _y, 0, 1.115f) <= 0.4f) return;

	int x = _x + 11;
	int y = _y + 5;
	random.SetRandomSeed(x, y);
	random.Next();
	int rnd = random.Random(1, 20);
	//wand spawns
	if (rnd != 1) return;

	float fOffset = random.ProceduralRandomf(x + 1, y, -4, 4);
	int intOffset = (int)rintf(fOffset);
	int pos_x = x + intOffset;
	int pos_y = y + intOffset;

	random.SetRandomSeed(pos_x, pos_y);
	if (random.Random(1, 100) >= 50) return;

	params.sCount++;
	writeInt(params.bytes, params.offset, pos_x);
	writeInt(params.bytes, params.offset, pos_y);
	writeByte(params.bytes, params.offset, TYPE_NIGHTMARE_WAND);
	writeInt(params.bytes, params.offset, 1);
	writeByte(params.bytes, params.offset, WAND_T10);

	Wand w = GetWandWithLevel(seed, pos_x, pos_y, 11, false, false);
	if (w.capacity > 30)
	{
	for (int i = 0; i < w.spellIdx; i++)
	{
		printf("%s ", SpellNames[w.spells[i]]);
	}
	printf("\n");
	printf("WAND - %i @ (%i %i) --- %.1f %i %i %i %i %i %i %.4f\n", seed, pos_x, pos_y, w.capacity, w.multicast, w.delay, w.reload, w.mana, w.regen, w.spread, w.speed);
}
*/

_compute static void spawnHellShop(int x, int y, const SpawnParams& params) {
	params.sCount++;
	writeInt(params.bytes, params.offset, x);
	writeInt(params.bytes, params.offset, y);
	writeByte(params.bytes, params.offset, TYPE_HELL_SHOP);
	writeInt(params.bytes, params.offset, 3);

	writeByte(params.bytes, params.offset, DATA_SPELL);
	writeShort(params.bytes, params.offset, GetRandomAction(params.seed, x, y, 10, 0));
}

_compute static Wand GetShopWand(NollaPRNG& random, double x, double y, int level, bool gen_spells) {
	random.SetRandomSeed(x, y);
	bool shuffle = random.Random(0, 100) <= 50;
	return GetWandWithLevel(random.world_seed, x, y, level, shuffle, false, gen_spells);
}

_compute void CheckMountains(const SpawnParams& params) {
	if (params.sCfg.pacifist) {
		for (int pw = params.sCfg.pwCenter.x - params.sCfg.pwWidth.x;
			pw <= params.sCfg.pwCenter.x + params.sCfg.pwWidth.x; pw++) {
			for (int hm_level = params.sCfg.minHMidx; hm_level <= min(params.sCfg.maxHMidx, pw == 0 ? 7 : 6);
				hm_level++) {
				int x = temple_x[hm_level] + chestOffsetX + params.currentBiome.map.w * 512 * pw;
				int y = temple_y[hm_level] + chestOffsetY;
				CheckNormalChestLoot(x, y, false, params);
			}
		}
	}

	if (params.sCfg.shopSpells || params.sCfg.shopWands) {
		NollaPRNG random(params.seed);
		int width = 132;
		constexpr int itemCount = 5;
		float stepSize = width / (float)itemCount;
		for (int pw = params.sCfg.pwCenter.x - params.sCfg.pwWidth.x;
			pw <= params.sCfg.pwCenter.x + params.sCfg.pwWidth.x; pw++) {
			for (int hm_level = params.sCfg.minHMidx; hm_level <= min(params.sCfg.maxHMidx, pw == 0 ? 7 : 6);
				hm_level++) {
				int x = temple_x[hm_level] + shopOffsetX + params.currentBiome.map.w * 512 * pw;
				int y = temple_y[hm_level] + shopOffsetY;
				int tier = temple_tiers[hm_level];
				random.SetRandomSeedInt(x, y);
				int sale_item = random.Random(0, itemCount - 1);
				bool wands = random.Random(0, 100) > 50;

				if (wands) {
					if (!params.sCfg.shopWands)
						continue;
#ifdef DO_WANDGEN
					params.sCount++;
					writeInt(params.bytes, params.offset, x);
					writeInt(params.bytes, params.offset, y);
					writeByte(params.bytes, params.offset, TYPE_HM_SHOP);
					int countOffset = params.offset;
					params.offset += 4;

					for (int i = 0; i < itemCount; i++) {
						Wand w = GetShopWand(random, round(x + i * stepSize), y, max(1, tier), params.sCfg.genSpells);
						writeByte(params.bytes, params.offset, DATA_WAND);
						params.bytes.is_safe("CheckMountains", "output", params.offset + 22 + w.spellCount * 3);
						cMemcpyU(params.bytes.ptr + params.offset, &w.capacity, 23);
						params.offset += 23;
						cMemcpyU(params.bytes.ptr + params.offset, w.spells, w.spellCount * 3);
						params.offset += w.spellCount * 3;
					}
					writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
#endif
				} else {
					if (!params.sCfg.shopSpells)
						continue;
					params.sCount++;
					writeInt(params.bytes, params.offset, x);
					writeInt(params.bytes, params.offset, y);
					writeByte(params.bytes, params.offset, TYPE_HM_SHOP);
					writeInt(params.bytes, params.offset, 6 * itemCount);

					for (int i = 0; i < itemCount; i++) {
						writeByte(params.bytes, params.offset, DATA_SPELL);
						writeShort(params.bytes, params.offset,
							GetRandomAction(random.world_seed, x + i * stepSize, y - 30, tier, 0));
						writeByte(params.bytes, params.offset, DATA_SPELL);
						writeShort(params.bytes, params.offset,
							GetRandomAction(random.world_seed, x + i * stepSize, y, tier, 0));
					}
				}
			}
		}
	}
}
_compute void CheckEyeRooms(const SpawnParams& params) {
	Vec2i positions[8] = {{-3992, 5380}, {-3971, 5397}, {-3949, 5414}, {-3926, 5428}, {-3758, 5424}, {-3735, 5410},
		{-3713, 5393}, {-3692, 5376}};
	if (params.sCfg.eyeRooms) {
		NollaPRNG random(params.seed);
		for (int pw = params.sCfg.pwCenter.x - params.sCfg.pwWidth.x;
			pw <= params.sCfg.pwCenter.x + params.sCfg.pwWidth.x; pw++) {
			int x = -3850 + pw * params.currentBiome.map.w * 512;
			int y = 5400;
			params.sCount++;
			writeInt(params.bytes, params.offset, x);
			writeInt(params.bytes, params.offset, y);
			writeByte(params.bytes, params.offset, TYPE_EYE_ROOM);
			writeInt(params.bytes, params.offset, 24);

			for (int i = 0; i < 8; i++) {
				Vec2i pos = positions[i] + Vec2i(pw * params.currentBiome.map.w * 512, 0);
				random.SetRandomSeedInt(pos.x, pos.y);
				writeByte(params.bytes, params.offset, DATA_SPELL);
				writeShort(params.bytes, params.offset, MakeRandomCard(random));
			}
		}
	}
}

_compute void CheckNightmareSpawnWands(const SpawnParams& params) {
	int wx = 703;
	float width = 132.f / 3;
	int wy = -94;
	int wtiers[] = {2, 2, 3, 1, 2, 3};
	//int wtypes[] = { WAND_T2, WAND_T2B, WAND_T3, WAND_T1NS, WAND_T2NS, WAND_T3NS };
	if (params.sCfg.nightmare) {
		writeInt(params.bytes, params.offset, wx);
		writeInt(params.bytes, params.offset, wy);
		writeByte(params.bytes, params.offset, TYPE_NIGHTMARE_WAND);
		int countOffset = params.offset;
		params.offset += 4;
		for (int i = 0; i < 3; i++) {
			Wand w = GetWandWithLevel(params.seed, wx + width * i, wy, wtiers[0], false, false, params.sCfg.genSpells);
			writeByte(params.bytes, params.offset, DATA_WAND);
			params.bytes.is_safe("CheckNightmareSpawnWands", "output", params.offset + 22 + w.spellCount * 3);
			cMemcpyU(params.bytes.ptr + params.offset, &w.capacity, 23);
			params.offset += 23;
			cMemcpyU(params.bytes.ptr + params.offset, w.spells, w.spellCount * 3);
			params.offset += w.spellCount * 3;
		}
		writeInt(params.bytes, countOffset, params.offset - countOffset - 4);
		params.sCount += 1;
	}
}

_compute void CheckSpawnables(const GeneratedBiome& s, SpawnParams& params) {
	// Resolve the spawn function table for the biome we are scanning
	BiomeSpawnFunctions& funcs = AllSpawnFunctions[params.currentBiome.bSec.b];
	if (funcs.init)
		funcs.init(params);

	// Traverse the Wang tile grid (note: y starts at -1 to include fringe)
	for (int y = -1; y < params.currentBiome.bSec.wang_h; y++) {
		for (int x = 0; x < params.currentBiome.bSec.wang_w; x++) {
			int tx = x * s.scope.ts.short_side_len; // tile origin in pixels (Wang short side)
			int ty = y * s.scope.ts.short_side_len;
			WangTileIndex tile = s.indices[y * params.currentBiome.bSec.wang_w + x];
			if (tile & 0x8000)
				continue; // blocked/invalid tile

			// Select the correct orientation array and strip flag bits from index
			const WangTile& t = tile & 0x4000 ? s.scope.ts.vTiles[tile & 0x3fff] : s.scope.ts.hTiles[tile & 0x3fff];
			for (int sIdx = 0; t.spawns[sIdx].i >= 0 && sIdx < _WangTileMaxSpawns; sIdx++) {
				// The spawn index may reference biome-local functions or fall back to global list 0
				auto spawn = t.spawns[sIdx].i >= funcs.count ?
								 AllSpawnFunctions[0].funcs[t.spawns[sIdx].i - funcs.count] :
								 funcs.funcs[t.spawns[sIdx].i];

				// Translate from tile-local to biome-local pixel coordinates
				int px = tx + t.spawns[sIdx].x;
				int py = ty + t.spawns[sIdx].y;
				py -= 4; // empirically matches in-game vertical offset for hooks
				if (px < 0 || py < 0 || px >= s.scope.bSec.map_w || py >= s.scope.bSec.map_h)
					continue;
				// Overlay-based culling for coalmine biome
				if (params.currentBiome.bSec.b == B_COALMINE && coalmine_overlay[py * 256 + px] == 1)
					continue; // solid blocks all spawns
				// Erase zone blocks only pixel scene spawns at tile corner markers
				if (params.currentBiome.bSec.b == B_COALMINE && t.spawns[sIdx].x == 0 && t.spawns[sIdx].y == 0 &&
					coalmine_overlay[py * 256 + px] == 2)
					continue;
				Vec2i global =
					GetGlobalPos(params.currentBiome.bSec.worldX, params.currentBiome.bSec.worldY, px * 10, py * 10);

				// Stay within the same biome when wrapping across the world map
				Vec2i chunk = GetLocalPos(global.x, global.y);
				Biome cBiome =
					s.scope.map.map[mod(chunk.y, s.scope.map.h) * s.scope.map.w + mod(chunk.x, s.scope.map.w)];
				if (cBiome != params.currentBiome.bSec.b)
					continue;

				// Repeat spawns for configured parallel worlds around the center
				for (int pwY = params.sCfg.pwCenter.y - params.sCfg.pwWidth.y;
					pwY <= params.sCfg.pwCenter.y + params.sCfg.pwWidth.y; pwY++) {
					for (int pwX = params.sCfg.pwCenter.x - params.sCfg.pwWidth.x;
						pwX <= params.sCfg.pwCenter.x + params.sCfg.pwWidth.x; pwX++) {
						Vec2i gp = GetGlobalPos(params.currentBiome.bSec.worldX + params.currentBiome.map.w * pwX,
							params.currentBiome.bSec.worldY + params.currentBiome.map.h * pwY, px * 10, py * 10);
						spawn(gp.x, gp.y, params);
						// Ensure we never overflow output buffer (is_safe checks live bounds)
						params.bytes.is_safe("CheckSpawnables", "output", max(0, params.offset - 1));
					}
				}
			}
		}
	}
}

_compute SpawnableBlock ParseSpawnableBlock(
	const uint8_t* block, MemSpan spawnables_block, const SpawnableConfig& sCfg, int seed, int sCount) {
	// Convert a raw byte buffer (block) into an array of Spawnable* pointers.
	// Each Spawnable starts with: x(4) y(4) sType(1) count(4) ...payload(count bytes)...
	int offset = 0;
	Spawnable** spawnables = (Spawnable**)spawnables_block.ptr;
	spawnables_block.is_safe("ParseSpawnableBlock", "pointer block", max(0, sCount * (int)sizeof(Spawnable*) - 1));
	for (int i = 0; i < sCount; i++) {
		Spawnable* s = (Spawnable*)(block + offset);
		spawnables[i] = s;
		// Step past header: 4+4+1 fields already included in Spawnable, but we also skip count(4)
		offset += 9;                // x(4) + y(4) + sType(1)
		int count = readInt(block, offset); // peek payload size
		offset += count;            // skip payload to the next entry
	}
	SpawnableBlock ret{seed, sCount, spawnables};
	return ret;
}
