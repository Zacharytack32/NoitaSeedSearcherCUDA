#pragma once

// ============================================================================
// Build configuration switches
// ----------------------------------------------------------------------------
// These compile-time flags enable/disable features and select a platform
// backend. Changing them does not alter simulator logic, only what code paths
// are compiled. Prefer toggling features via runtime config when possible.
// ============================================================================

// Define exactly one backend:
#define BACKEND_CPU              // run on CPU threads (default)
//#define BACKEND_CUDA           // run on CUDA device (requires NVIDIA GPU)

// Enable extra device/worker logging to stdout (for debugging)
#define DEVICE_LOGGING

// Optional features (uncomment to enable):
//#define IMAGE_OUTPUT           // write full RGB map images to output buffer (large)
//#define SEEDS_AS_TRIES 1434608330 // treat worldSeed as retry count for worldgen (debug)
#define DO_WORLDGEN              // generate biome maps
#define DO_WANDGEN               // generate wand stats/payloads when requested
#define DO_SPELLGEN              // enable spell generation/lookups
// //#define REALTIME_SEEDS       // pick seeds from wall-clock time (demo mode)
//#define SFML                    // enable SFML UI (if integrated)
