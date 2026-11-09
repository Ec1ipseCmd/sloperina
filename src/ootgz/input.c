#include "input.h"

#include "got.h"
#include "sys.h"

/* nn::hid state structs (Switch 1 layout: sampling number, buttons, two sticks, attributes;
 * 0x28 bytes). We only touch the leading fields and read one state into a larger buffer, so
 * a longer Switch 2 struct is harmless. VERIFY the field order on hardware (BUILDING.md). */
typedef struct {
  int64_t  sampling_number;
  uint64_t buttons;
  int32_t  lx, ly, rx, ry;
  uint32_t attributes;
} npad_state_t;

#define NPAD_STATE_BUF 0x100

/* nn::hid::detail::GetNpadStates(int *out_count, NpadXState *out, int count, const NpadIdType &id),
 * exported by sdk. The detail versions are what the public ones call, so they keep working
 * when the public ones are interposed below. */
#define DETAIL(style) "_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_" style "EiRKj"
void hid_detail_fullkey(int *n, void *out, int count, const uint32_t *id)
  __asm__(DETAIL("16NpadFullKeyState"));
void hid_detail_handheld(int *n, void *out, int count, const uint32_t *id)
  __asm__(DETAIL("17NpadHandheldState"));
void hid_detail_joydual(int *n, void *out, int count, const uint32_t *id)
  __asm__(DETAIL("16NpadJoyDualState"));

enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };

static uint32_t held, pressed, repeat_mask, fired;
static int32_t sticks[4];
static int repeat_timer;
bool input_block_game;
uint32_t input_hide_from_game;

static void read_one(void (*fn)(int *, void *, int, const uint32_t *), uint32_t id)
{
  uint64_t buf[NPAD_STATE_BUF / 8] = { 0 };
  int n = 0;
  fn(&n, buf, 1, &id);
  if (n < 1)
    return;
  npad_state_t *s = (npad_state_t *)buf;
  if (!(s->attributes & 1)) /* IsConnected */
    return;
  held |= (uint32_t)s->buttons;
  if (s->lx || s->ly)
    sticks[0] = s->lx, sticks[1] = s->ly;
  if (s->rx || s->ry)
    sticks[2] = s->rx, sticks[3] = s->ry;
}

void input_update(void)
{
  uint32_t prev = held;
  held = 0;
  sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0;
  read_one(hid_detail_fullkey, NPAD_NO1);
  read_one(hid_detail_joydual, NPAD_NO1);
  read_one(hid_detail_handheld, NPAD_HANDHELD);
  pressed = held & ~prev;

  /* gz-style repeat: 12 frames before the first repeat, then every 3 */
  fired = pressed;
  if (pressed) {
    repeat_mask = pressed;
    repeat_timer = 12;
  } else if (held & repeat_mask) {
    repeat_mask &= held;
    if (--repeat_timer <= 0) {
      repeat_timer = 3;
      fired = repeat_mask;
    }
  } else {
    repeat_mask = 0;
  }
}

uint32_t input_held(void) { return held; }
uint32_t input_pressed(void) { return pressed; }

bool input_combo(uint32_t mask)
{
  return (held & mask) == mask && (pressed & mask);
}

bool input_repeat(uint32_t button)
{
  return (fired & button) != 0;
}

void input_sticks(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry)
{
  *lx = sticks[0], *ly = sticks[1], *rx = sticks[2], *ry = sticks[3];
}

/* --- hiding input from the game -------------------------------------------------------------
 * main's calls to the public nn::hid::GetNpadStates overloads go through its GOT; module init
 * points those slots at the wrappers below (got.c). While the menu is open the game then sees
 * the controller idle. Only the buttons and sticks of each state are cleared (Switch 1 layout,
 * 0x28 bytes per state: a longer Switch 2 state is still only written inside the buffer). */
#define PUBLIC(style) "_ZN2nn3hid13GetNpadStatesEPNS0_" style "EiRKj"
typedef int (*hid_public_t)(void *out, int count, const uint32_t *id);

static const char *const hid_public_names[] = {
  PUBLIC("16NpadFullKeyState"), PUBLIC("17NpadHandheldState"), PUBLIC("16NpadJoyDualState"),
  PUBLIC("16NpadJoyLeftState"), PUBLIC("17NpadJoyRightState"),
};
#define N_HID (sizeof(hid_public_names) / sizeof(*hid_public_names))
/* the sdk functions (imported by this module, so they bypass main's GOT) */
int hid_public_0(void *, int, const uint32_t *) __asm__(PUBLIC("16NpadFullKeyState"));
int hid_public_1(void *, int, const uint32_t *) __asm__(PUBLIC("17NpadHandheldState"));
int hid_public_2(void *, int, const uint32_t *) __asm__(PUBLIC("16NpadJoyDualState"));
int hid_public_3(void *, int, const uint32_t *) __asm__(PUBLIC("16NpadJoyLeftState"));
int hid_public_4(void *, int, const uint32_t *) __asm__(PUBLIC("17NpadJoyRightState"));

bool input_force_active;
uint32_t input_force_buttons;
bool input_force_sticks[2];
int32_t input_force_stick[4];

static void hide(void *out, int count)
{
  if (!input_block_game && !input_hide_from_game && !input_force_active)
    return;
  for (int i = 0; i < count; i++) {
    npad_state_t *s = (npad_state_t *)((char *)out + i * 0x28);
    if (input_block_game) {
      s->buttons = 0;
      s->lx = s->ly = s->rx = s->ry = 0;
    } else {
      if (input_force_active) {
        s->buttons |= input_force_buttons;
        if (input_force_sticks[0])
          s->lx = input_force_stick[0], s->ly = input_force_stick[1];
        if (input_force_sticks[1])
          s->rx = input_force_stick[2], s->ry = input_force_stick[3];
      }
      s->buttons &= ~(uint64_t)input_hide_from_game;
    }
  }
}

#define WRAP(n)                                                    \
  static int hid_wrap_##n(void *out, int count, const uint32_t *id) \
  {                                                                 \
    int r = hid_public_##n(out, count, id);                         \
    hide(out, count);                                               \
    return r;                                                       \
  }
WRAP(0) WRAP(1) WRAP(2) WRAP(3) WRAP(4)

static void *const hid_wraps[N_HID] = { hid_wrap_0, hid_wrap_1, hid_wrap_2, hid_wrap_3, hid_wrap_4 };

const char *input_hook_game(uintptr_t main_base)
{
  const char *err = 0;
  for (unsigned i = 0; i < N_HID; i++) {
    const char *e = got_hook(main_base, hid_public_names[i], hid_wraps[i]);
    if (e)
      err = e;
  }
  return err;
}
