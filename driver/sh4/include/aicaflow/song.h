#ifndef AICAFLOW_SONG_H
#define AICAFLOW_SONG_H

#include <stdint.h>

#include <aicaflow/bank.h>
#include <aicaflow/host.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A song (AFM, docs/design/song-sections.md) is sections played in an
 * order, each joined to the next by its NEXT: a continue join goes on with the
 * same executor and voices, as LOOP did; a new-play join starts the next
 * section with voices of its own while the old one plays its tail. At the
 * end of the order a song goes on at its loop index, reported as a loop.
 *
 * A song loads like a flow, against its banks in slot order (one setup table,
 * relocations and bank slots for the whole song), and plays through the
 * instance API: activate, stop, pause, gain, tempo, lanes, status and the end
 * and loop callbacks take a song's instance like a flow's. A song with a
 * new-play join plays from a pool of voices that activation grants, and from
 * a runtime block of that instance's own in AICA RAM; any number of
 * instances of one song may play at once.
 *
 * A song's status position is AFX_SONG_POSITION(order position, stream offset)
 * of its newest section; afx_instance_rebuild() takes such a position back
 * and resumes there (restore states only for a song without a pool). A
 * tail that rang at a pause is not resumed. afx_instance_seek() needs an AFC,
 * which songs have none of. */
int afx_song_load(const afx_bank_t *const banks[], uint32_t bank_count, const void *afm,
                  uint32_t afm_size, afx_asset_t *out_song);
/* Restarts a paused song's instance at the start of an order position. */
int afx_song_seek(afx_instance_t instance, uint32_t order_position);

#ifdef __cplusplus
}
#endif

#endif
