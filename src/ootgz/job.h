/* Multi-step changes that run on the game's thread, one step per frame.
 *
 * Native replacement for patch.py's submit()/wait_handled(): the game queues GameData
 * writes (SetInt) until the next flush, so steps that depend on each other (give an item,
 * then create it on Link, then redraw the HUD) still run on separate frames. Jobs run in
 * order; submitting a job whose key is already queued replaces the queued one (scrolling
 * through wallet sizes runs only the last). */
#ifndef JOB_H
#define JOB_H

#include <stdbool.h>
#include <stdint.h>

typedef struct job job_t;
/* Called once per frame with j->step = 0, 1, ...; return true when finished. */
typedef bool (*job_fn_t)(job_t *j);

struct job {
  uint32_t  key;     /* jobs with the same key replace each other while queued */
  job_fn_t  fn;
  int       step;
  int       i;       /* free for the job */
  intptr_t  arg[4];
};

bool job_submit(uint32_t key, job_fn_t fn, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3);
bool job_pending(uint32_t key); /* queued or running */
void job_frame(void);           /* run one step; called from the frame hook */

#endif
