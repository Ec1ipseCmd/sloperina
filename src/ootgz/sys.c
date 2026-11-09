#include "sys.h"

#define PREFIX "sloperinaGZ: " /* on each line of the emulator's / Atmosphère's log */
#define PREFIX_LEN (sizeof(PREFIX) - 1)

/* --- system calls ---------------------------------------------------------------------- */

uint32_t sys_query_memory(sys_meminfo_t *info, uint64_t addr)
{
  register uint64_t x0 __asm__("x0") = (uint64_t)info;
  register uint64_t x1 __asm__("x1");
  register uint64_t x2 __asm__("x2") = addr;
  __asm__ volatile("svc 0x6" : "+r"(x0), "=r"(x1), "+r"(x2) : : "memory", "x3", "x4", "x5",
                   "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
                   "x17", "x18");
  return (uint32_t)x0;
}

void sys_sleep_ns(int64_t ns)
{
  register uint64_t x0 __asm__("x0") = (uint64_t)ns;
  __asm__ volatile("svc 0xB" : "+r"(x0) : : "memory", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                   "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18");
}

bool sys_get_info(uint32_t type, uint64_t sub, uint64_t *out)
{
  register uint64_t x0 __asm__("x0");
  register uint64_t x1 __asm__("x1") = type;
  register uint64_t x2 __asm__("x2") = 0xFFFF8001; /* this process */
  register uint64_t x3 __asm__("x3") = sub;
  __asm__ volatile("svc 0x29" : "=r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory", "x4", "x5", "x6", "x7",
                   "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18");
  if ((uint32_t)x0 != 0)
    return false;
  *out = x1;
  return true;
}

static void sys_output_debug_string(const char *s, size_t len)
{
  register uint64_t x0 __asm__("x0") = (uint64_t)s;
  register uint64_t x1 __asm__("x1") = len;
  __asm__ volatile("svc 0x27" : "+r"(x0), "+r"(x1) : : "memory", "x2", "x3", "x4", "x5", "x6",
                   "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                   "x18");
}

uint64_t sys_tick(void)
{
  uint64_t t;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
  return t;
}

uint64_t sys_tick_freq(void)
{
  uint64_t f;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}

/* --- memory ---------------------------------------------------------------------------- */

static sys_meminfo_t last_region;

bool sys_mapped(uintptr_t addr, size_t size, uint32_t perm)
{
  if (addr < 0x1000 || addr + size < addr)
    return false;
  sys_meminfo_t *m = &last_region;
  if (!(m->size && addr >= m->addr && addr + size <= m->addr + m->size)) {
    if (sys_query_memory(m, addr) != 0) {
      m->size = 0;
      return false;
    }
  }
  if (m->type == 0 || (m->perm & perm) != perm)
    return false;
  return addr + size <= m->addr + m->size;
}

uintptr_t sys_ptr(uintptr_t addr)
{
  if ((addr & 7) || !sys_mapped(addr, 8, SYS_PERM_R))
    return 0;
  return *(volatile uintptr_t *)addr;
}

bool sys_read32(uintptr_t addr, uint32_t *out)
{
  if ((addr & 3) || !sys_mapped(addr, 4, SYS_PERM_R))
    return false;
  *out = *(volatile uint32_t *)addr;
  return true;
}

void sys_regions(uint32_t perm, void (*fn)(uintptr_t, size_t, void *), void *ctx)
{
  sys_meminfo_t m;
  uint64_t addr = 0;
  for (int guard = 0; guard < 100000; guard++) {
    if (sys_query_memory(&m, addr) != 0 || m.size == 0)
      break;
    if (m.type != 0 && (m.perm & perm) == perm && !(m.attr & 0x1 /* locked/borrowed */))
      fn((uintptr_t)m.addr, (size_t)m.size, ctx);
    uint64_t next = m.addr + m.size;
    if (next <= addr)
      break;
    addr = next;
  }
}

/* --- logging --------------------------------------------------------------------------- */

static char log_ring[LOG_LINES][LOG_LINE_LEN];
static int log_head;

/* Lines waiting to be written to the log file (settings.c, on the game's thread): log_printf can be
 * called from the drawing thread too, so the buffer has a small lock. Kept from the start, so the lines
 * before the SD card is mounted get written too; if it fills up before then, the newest are dropped. */
#define FILE_PENDING 32768
static char file_pending[FILE_PENDING];
static size_t file_len;
static volatile int file_lock;

static void file_add(const char *s, size_t n)
{
  while (__atomic_test_and_set(&file_lock, __ATOMIC_ACQUIRE))
    ;
  uint64_t ms = sys_tick() * 1000 / sys_tick_freq();
  char stamp[24];
  int sl = snprintf(stamp, sizeof(stamp), "[%5u.%03u] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
  if (sl > 0 && file_len + (size_t)sl + n + 1 < FILE_PENDING) {
    memcpy(file_pending + file_len, stamp, (size_t)sl);
    memcpy(file_pending + file_len + sl, s, n);
    file_len += (size_t)sl + n;
    file_pending[file_len++] = '\n';
  }
  __atomic_clear(&file_lock, __ATOMIC_RELEASE);
}

size_t log_take_pending(char *out, size_t cap)
{
  while (__atomic_test_and_set(&file_lock, __ATOMIC_ACQUIRE))
    ;
  size_t n = file_len < cap ? file_len : cap;
  memcpy(out, file_pending, n);
  memmove(file_pending, file_pending + n, file_len - n);
  file_len -= n;
  __atomic_clear(&file_lock, __ATOMIC_RELEASE);
  return n;
}

void log_printf(const char *fmt, ...)
{
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + PREFIX_LEN, sizeof(buf) - PREFIX_LEN, fmt, ap);
  va_end(ap);
  memcpy(buf, PREFIX, PREFIX_LEN);
  if (n < 0)
    return;
  size_t len = PREFIX_LEN + ((size_t)n < sizeof(buf) - PREFIX_LEN - 1 ? (size_t)n : sizeof(buf) - PREFIX_LEN - 1);
  sys_output_debug_string(buf, len);

  log_head = (log_head + 1) % LOG_LINES;
  size_t l = len - PREFIX_LEN < LOG_LINE_LEN - 1 ? len - PREFIX_LEN : LOG_LINE_LEN - 1;
  memcpy(log_ring[log_head], buf + PREFIX_LEN, l);
  log_ring[log_head][l] = 0;
  file_add(buf + PREFIX_LEN, len - PREFIX_LEN);
}

void log_raw(const char *s)
{
  char buf[160] = PREFIX;
  size_t n = PREFIX_LEN;
  while (*s && n < sizeof(buf) - 1)
    buf[n++] = *s++;
  sys_output_debug_string(buf, n);
}

const char *log_line(int i)
{
  if (i < 0 || i >= LOG_LINES)
    return 0;
  const char *s = log_ring[(log_head - i + LOG_LINES) % LOG_LINES];
  return s[0] ? s : 0;
}
