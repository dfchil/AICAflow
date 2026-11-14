#ifndef AICAFLOW_SCENE_H
#define AICAFLOW_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#include <aicaflow/host.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An AFS scene is the banks and flows a program loads together, placed in one
 * AICA block whose layout the authoring tool planned (afx_scene). Nothing in a
 * block moves and a block is freed whole, so scenes loaded and released in
 * turn do not fragment the asset arena. Plain asset and bank loads keep
 * working beside them. */
typedef uint32_t afx_scene_t;

/* Scenes loaded at once, those released but still playing included. */
#define AFX_MAX_SCENES 16u

typedef struct {
    uint32_t kind; /* AFX_SCENE_BANK, _FLOW, _SONG (an AFM, found by name as a flow is), _SLOT or _RING. */
    const char *name; /* The entry's name, "" for none; valid while the scene is loaded. */
    afx_asset_t asset; /* A bank's or a flow's handle, a slot's loaded flow, or zero. */
    uint32_t addr; /* AICA address of the entry's range in the block. */
    uint32_t bytes; /* Size of that range in bytes. */
    uint32_t ticks; /* A flow's authored length in ticks; zero for other kinds. */
    bool loops; /* The flow loops; see afx_asset_loops(). */
} afx_scene_info_t;

/* Validates an AFS file and loads it into one allocation of its block size,
 * every bank and flow at its planned offset, banks by DMA. A flow's bank slot
 * takes the bank with that identity in this scene, or else in one of parents,
 * such as the core bank of a permanent scene. data may be freed on return. A
 * failure loads nothing. */
int afx_scene_load(const void *data, uint32_t size, const afx_scene_t *parents,
                   uint32_t parent_count, afx_scene_t *out_scene);
/* The flow named name, or the flow loaded into the slot named name. */
int afx_scene_find(afx_scene_t scene, const char *name, afx_asset_t *out_flow);
/* Entry index of the scene, as the file lists them; -AFX_BAD_BOUNDS past the
 * last one. A Music Test lists the flows this way. */
int afx_scene_entry(afx_scene_t scene, uint32_t index, afx_scene_info_t *out_info);
/* Loads an AFB bank (or none, with a null bank) and an AFX flow into the slot
 * named slot: the bank's payload at the slot's start, the flow after it,
 * never through the free list. The flow's bank slots take this bank by its
 * identity, or else the scene's banks and its parents'. What the slot held
 * before must be idle, as afx_asset_release() would free it at once: no
 * instance plays its flow and no other flow binds its bank. Otherwise this
 * returns -AFX_ASSET_REFERENCED and the slot keeps it; any other failure
 * leaves the slot empty. */
int afx_scene_slot_load(afx_scene_t scene, const char *slot, const void *bank, uint32_t bank_size,
                        const void *flow, uint32_t flow_size, afx_asset_t *out_flow);
/* Gives the scene up: its flows and banks are released as by
 * afx_asset_release(), and its block is freed once no instance plays any of
 * its flows and no flow of another scene binds any of its banks. The scene's
 * handle goes stale at once. Refused with -AFX_ASSET_REFERENCED while the DSP
 * uses the scene's ring; disable it first.
 * A scene's flows and banks belong to it: afx_asset_free() and
 * afx_asset_release() refuse them with -AFX_ASSET_REFERENCED. */
int afx_scene_release(afx_scene_t scene);

#ifdef __cplusplus
}
#endif

#endif
