// tire_state.h: the mod's state as it publishes it for other mods (a gauge cluster that shows the tire pressure level),
// in a block of shared memory "Local\SnowRunnerTirePressure.State.<process id>", written at the end of every pass,
// about 20 times a second. The block is 64 bytes, version 1; a later version only adds to its end. The writer makes
// the sequence odd while it writes; a reader copies while it is even and unchanged (TireStateCopy), and takes a tick
// older than two seconds as the mod gone. The mapping lives on while either side holds it, so a reader never holds a
// pointer into this mod's file.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const uint32_t kTireMagic = 0x45524954u; // "TIRE"
static const uint32_t kTireVersion = 1;

// The flags.
enum : uint32_t
{
    kTireVehicle = 1u << 0,        // a truck is driven; vehicle names it and the rest describes its tires
    kTireChanging = 1u << 1,       // the pressure is on its way to the chosen mode's
    kTireHeld = 1u << 2,           // the change waits for the truck to stand still (StandStill)
    kTireTooFast = 1u << 3,        // the truck is too fast for its tire pressure: the tires take damage
    kTireWorn = 1u << 4,           // a tire of the truck is worn out (flat, without grip)
    kTireHardMode = 1u << 5,       // hard mode is on
    kTireOutsideFromMod = 1u << 6, // the outside temperature comes from another mod's block, not from the ini
};

// The block, version 1.
struct TireState
{
    uint32_t magic, version, size;
    volatile long sequence; // odd while the writer writes
    uint64_t tick;          // GetTickCount64 at the write
    uint64_t vehicle;       // the truck driven, 0 none
    int32_t mode;           // the chosen mode: 0 Low, 1 Reduced, 2 Normal, 3 Increased
    int32_t modes;          // the modes in use: 3 (Low to Normal) or 4
    float pressure;         // the tires' pressure as a share of Normal's, where it stands (Low 0.45, Reduced 0.65, Normal 1, Increased 1.25)
    float pressureAir;      // the same as the outside temperature leaves it, what the grip goes by
    float target;           // the chosen mode's pressure, where the tires are headed
    float outsideC;         // the outside temperature in force, degrees C
    uint32_t flags;
    int32_t wear;           // the most worn tire's damage in percent of what it takes, -1 not known
};
static_assert(sizeof(TireState) == 64 && offsetof(TireState, tick) == 16 && offsetof(TireState, vehicle) == 24 && offsetof(TireState, mode) == 32 &&
              offsetof(TireState, pressure) == 40 && offsetof(TireState, outsideC) == 52 && offsetof(TireState, flags) == 56 && offsetof(TireState, wear) == 60,
              "the layout, version 1");

struct TireStateWriter
{
    HANDLE mapping;
    volatile TireState *block;
};

// Makes the block and its fixed fields; false without it, and the mod then publishes nothing.
inline bool TireStateOpen(TireStateWriter &w)
{
    w = TireStateWriter{};
    wchar_t name[64];
    swprintf(name, 64, L"Local\\SnowRunnerTirePressure.State.%lu", GetCurrentProcessId());
    w.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TireState), name);
    if (w.mapping) w.block = (volatile TireState *)MapViewOfFile(w.mapping, FILE_MAP_WRITE, 0, 0, sizeof(TireState));
    if (!w.block)
    {
        if (w.mapping) CloseHandle(w.mapping);
        w = TireStateWriter{};
        return false;
    }
    memset((void *)w.block, 0, sizeof(TireState));
    w.block->magic = kTireMagic;
    w.block->version = kTireVersion;
    w.block->size = sizeof(TireState);
    return true;
}

// Writes v's fields (magic, version, size and sequence are the block's own) under an odd sequence, with the tick of now.
inline void TireStateWrite(TireStateWriter &w, const TireState &v)
{
    if (!w.block) return;
    InterlockedIncrement(&w.block->sequence); // odd: a reader waits
    MemoryBarrier();
    w.block->tick = GetTickCount64();
    w.block->vehicle = v.vehicle;
    w.block->mode = v.mode;
    w.block->modes = v.modes;
    w.block->pressure = v.pressure;
    w.block->pressureAir = v.pressureAir;
    w.block->target = v.target;
    w.block->outsideC = v.outsideC;
    w.block->flags = v.flags;
    w.block->wear = v.wear;
    MemoryBarrier();
    InterlockedIncrement(&w.block->sequence); // even again
}

inline void TireStateClose(TireStateWriter &w)
{
    if (w.block) UnmapViewOfFile((const void *)w.block);
    if (w.mapping) CloseHandle(w.mapping);
    w = TireStateWriter{};
}

// A reader's consistent copy of the block, or false while the writer is in the middle of a write or the block is not
// this mod's (the offline test reads its own this way, and another mod can take this function as it is).
inline bool TireStateCopy(const volatile TireState *from, TireState &to)
{
    for (int tries = 0; tries < 8; tries++)
    {
        const long before = from->sequence;
        if (before & 1) { Sleep(0); continue; }
        memcpy(&to, (const void *)from, sizeof to);
        MemoryBarrier();
        if (from->sequence != before) continue;
        return to.magic == kTireMagic && to.version >= 1 && to.size >= sizeof(TireState);
    }
    return false;
}
