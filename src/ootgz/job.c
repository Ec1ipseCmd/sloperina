#include "job.h"

#include "sys.h"

#define JOB_MAX 32
#define JOB_STEP_LIMIT 600 /* frames; a job still running after this is abandoned */

static job_t queue[JOB_MAX];
static int count;

bool job_submit(uint32_t key, job_fn_t fn, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3)
{
  job_t j = { .key = key, .fn = fn, .arg = { a0, a1, a2, a3 } };
  /* replace a queued (not yet started) job with the same key */
  for (int i = 1; i < count; i++)
    if (queue[i].key == key) {
      queue[i] = j;
      return true;
    }
  if (count && queue[0].key == key && queue[0].step == 0) {
    queue[0] = j;
    return true;
  }
  if (count >= JOB_MAX) {
    log_printf("job queue full, dropped %08x", key);
    return false;
  }
  queue[count++] = j;
  return true;
}

bool job_pending(uint32_t key)
{
  for (int i = 0; i < count; i++)
    if (queue[i].key == key)
      return true;
  return false;
}

void job_frame(void)
{
  if (!count)
    return;
  job_t *j = &queue[0];
  bool done = j->fn(j);
  j->step++;
  if (!done && j->step > JOB_STEP_LIMIT) {
    log_printf("job %08x abandoned after %d steps", j->key, j->step);
    done = true;
  }
  if (done) {
    for (int i = 1; i < count; i++)
      queue[i - 1] = queue[i];
    count--;
  }
}
