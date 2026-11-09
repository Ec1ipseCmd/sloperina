#include "enemy_health.h"
#include "actors.h"
#include "world_view.h"
#include "gamedata.h"
#include "sys.h"

#define GD_SEQUENCE_IS_PAUSE 0x604AEBE1u
#define GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT 0x64A61098u

static enemy_label_t published_labels[ENEMY_LABEL_MAX];
static int published_count;
static unsigned char publishing;

typedef struct {
  cam_t camera;
  enemy_label_t labels[ENEMY_LABEL_MAX];
  float distances[ENEMY_LABEL_MAX];
  int count;
} health_frame_t;

static void enemy(uintptr_t actor, const char *name, void *arg)
{
  health_frame_t *frame = arg;
  for (int i = 0; i < 6; i++)
    if (name[i] != "Enemy_"[i])
      return;
  int32_t current, maximum;
  vec3f_t head;
  if (!actor_health(actor, &current, &maximum) || !current || !actor_label_position(actor, &head))
    return;
  float world[3] = { head.x, head.y, head.z }, view[3], x, y;
  to_view(&frame->camera, world, view);
  float distance = view[0] * view[0] + view[1] * view[1] + view[2] * view[2];
  if (!(view[2] > 0.05f) || !(distance < 60 * 60))
    return;
  to_screen(&frame->camera, view, &x, &y);
  if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1))
    return;
  int at = frame->count;
  while (at > 0 && frame->distances[at - 1] > distance)
    at--;
  if (at >= ENEMY_LABEL_MAX)
    return;
  if (frame->count < ENEMY_LABEL_MAX)
    frame->count++;
  for (int i = frame->count - 1; i > at; i--) {
    frame->labels[i] = frame->labels[i - 1];
    frame->distances[i] = frame->distances[i - 1];
  }
  frame->distances[at] = distance;
  enemy_label_t *label = &frame->labels[at];
  label->x = (uint16_t)(x * 65535);
  label->y = (uint16_t)(y * 65535);
  snprintf(label->text, sizeof(label->text), "%d/%d", (int)current, (int)maximum);
}

void enemy_health_frame(bool visible)
{
  health_frame_t frame = {0};
  bool paused = false;
  int32_t requests = 0;
  if (visible) {
    gd_get_bool(GD_SEQUENCE_IS_PAUSE, &paused);
    gd_get_int(GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT, &requests);
    if (!paused && !requests && read_camera(&frame.camera) && !actors_each(enemy, &frame))
      frame.count = 0; /* A changing/broken list must not leave stale labels. */
  }
  /* Neither thread waits on the other. Only a small mod-owned snapshot is shared. */
  if (!__atomic_test_and_set(&publishing, __ATOMIC_ACQUIRE)) {
    memcpy(published_labels, frame.labels, sizeof(published_labels));
    published_count = frame.count;
    __atomic_clear(&publishing, __ATOMIC_RELEASE);
  }
}

int enemy_health_copy(enemy_label_t out[ENEMY_LABEL_MAX])
{
  if (__atomic_test_and_set(&publishing, __ATOMIC_ACQUIRE))
    return 0;
  int count = published_count;
  memcpy(out, published_labels, sizeof(published_labels));
  __atomic_clear(&publishing, __ATOMIC_RELEASE);
  return count;
}
