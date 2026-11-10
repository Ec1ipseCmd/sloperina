/* Execute nvn.c's actual fill functions with a deferred, fenced mock GPU. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "void_view.h"
uintptr_t camera(void) { return 0; }
bool sys_mapped(uintptr_t a,size_t n,uint32_t p) { (void)a;(void)n;(void)p;return false; }
typedef struct { uint32_t *pixels;size_t bytes;bool flushed; } nvn_obj_t;
typedef int nvn_queue_t;
typedef struct { uint32_t pixels[4096][4096];int w,h; } nvn_texture_t;
typedef struct { int x,y,z,w,h,d; } nvn_region_t;
typedef struct { int x,y,w,h; } nvn_rect_t;
typedef uintptr_t nvn_addr_t;
typedef uint64_t nvn_handle_t;
#define PAGE_SIZE 4096
#define CMD_SIZE 0x100000
#define WORLD_BUFFER_MAX (2*1024*1024)
#define CTRL_SIZE 8192
#define POOL_CPU_CACHED 4
#define POOL_GPU_UNCACHED 16
#define SYNC_ALL_GPU_COMMANDS_COMPLETE 0
static nvn_obj_t cmd_buffer,command_pool,slot_sync[2];
static char control_mem[CTRL_SIZE*2];
static bool slot_used[2],wait_ok=true,waited,failed,fail_pool,fail_alloc,have_flush=true;
static bool queue_flushed,have_queue_flush=true;
static int swapchain_format=37,invalidations,flushes,uploads,waits,fail_wait;
static bool pending;
static size_t live_bytes,peak_bytes,allocation_limit=2*1024*1024,max_allocation=2*1024*1024;
static unsigned allocations;
static size_t read_bytes;
static unsigned read_commands;
static size_t command_limit=CMD_SIZE;
static nvn_obj_t *mock_pool;
static void *device;
static uint8_t *world_reserved;
static bool is_rgba8(int n) { return n==37; }
static void log_printf(const char *fmt,...) { (void)fmt; }
static void overlay_fail(const char *message) { (void)message;failed=true; }
static void *test_alloc(size_t alignment,size_t size)
{
  assert(alignment==PAGE_SIZE && !(size%PAGE_SIZE));allocations++;
  if (fail_alloc || size>max_allocation || live_bytes+size>allocation_limit) return NULL;
  void *p=malloc(size);assert(p);live_bytes+=size;
  if (live_bytes>peak_bytes) peak_bytes=live_bytes;
  return p;
}
static void test_free(void *p) { if (p) { free(p);live_bytes=0; } }
#define free test_free
#define aligned_alloc test_alloc
static bool make_pool(nvn_obj_t *p,void *data,size_t size,int flags)
{ assert(flags==(POOL_CPU_CACHED|POOL_GPU_UNCACHED));*p=(nvn_obj_t){data,size,false};mock_pool=p;return !fail_pool; }
static struct { nvn_texture_t *tex;nvn_obj_t *pool;nvn_region_t r;size_t offset;bool upload; } ops[8192],queued[16384];
static unsigned queued_count;
static int recording_slot,pending_slot;
static unsigned operations;
static void add_command(nvn_obj_t *b,const nvn_obj_t *p,ptrdiff_t at,size_t n)
{ (void)b;(void)p;assert((at==0 || at==CMD_SIZE) && n==CMD_SIZE);recording_slot=(int)(at/CMD_SIZE); }
static void add_control(nvn_obj_t *b,void *p,size_t n)
{ (void)b;assert(p==control_mem+recording_slot*CTRL_SIZE && n==CTRL_SIZE); }
static void begin(nvn_obj_t *b) { (void)b;assert(!pending || recording_slot!=pending_slot);operations=0;waited=false;queue_flushed=false; }
static size_t command_free(const nvn_obj_t *b) { (void)b;size_t used=operations*256;return used<command_limit?command_limit-used:0; }
static nvn_handle_t end(nvn_obj_t *b) { (void)b;return operations; }
static nvn_addr_t address(const nvn_obj_t *p) { return (uintptr_t)p->pixels; }
static int width(const nvn_texture_t *t) { return t->w; }
static void record(nvn_texture_t *t,nvn_addr_t a,const nvn_region_t *r,bool upload)
{
  nvn_obj_t *p=mock_pool;size_t offset=a-(uintptr_t)p->pixels;
  assert(operations<8192 && r->x>=0 && r->y>=0 && r->x+r->w<=t->w && r->y+r->h<=t->h);
  assert(r->z==0 && r->d==1 && offset+(size_t)r->w*r->h*4<=p->bytes);
  if (upload) { assert(p->flushed);uploads++; }
  else { read_bytes+=(size_t)r->w*r->h*4;read_commands++; }
  ops[operations].tex=t;ops[operations].pool=p;ops[operations].r=*r;ops[operations].offset=offset;
  ops[operations++].upload=upload;
}
static void to_buffer(nvn_obj_t *b,const nvn_texture_t *t,const void *v,const nvn_region_t *r,nvn_addr_t a,int flags)
{ (void)b;assert(!v && !flags);record((nvn_texture_t *)t,a,r,false); }
static void to_texture(nvn_obj_t *b,nvn_addr_t a,const nvn_texture_t *t,const void *v,const nvn_region_t *r,int flags)
{ (void)b;assert(!v && !flags);record((nvn_texture_t *)t,a,r,true); }
static void submit(nvn_queue_t *q,int n,const nvn_handle_t *h)
{
  (void)q;assert(n==1 && *h==operations && queued_count+operations<=16384);
  memcpy(queued+queued_count,ops,operations*sizeof(*ops));queued_count+=operations;
  pending=true;pending_slot=recording_slot;
}
static void fence(nvn_queue_t *q,nvn_obj_t *sync,int condition,int flags)
{ (void)q;assert(sync==slot_sync+recording_slot && condition==0 && !flags); }
static bool slot_finished(int slot)
{
  assert(slot==recording_slot && slot_used[slot] && queue_flushed);
  assert(pending);waits++;
  if (!wait_ok || waits==fail_wait) return false;
  for (unsigned i=0;i<queued_count;i++) {
    nvn_region_t r=queued[i].r;nvn_obj_t *p=queued[i].pool;nvn_texture_t *t=queued[i].tex;
    uint32_t *pixels=p->pixels+queued[i].offset/4;
    for (int y=0;y<r.h;y++) for (int x=0;x<r.w;x++) {
      if (queued[i].upload) t->pixels[y+r.y][x+r.x]=pixels[y*r.w+x];
      else pixels[y*r.w+x]=t->pixels[y+r.y][x+r.x];
    }
    if (!queued[i].upload) p->flushed=false;
  }
  waited=true;pending=false;queued_count=0;return true;
}
static void invalidate(const nvn_obj_t *p,ptrdiff_t offset,size_t bytes)
{ assert(waited && !offset && bytes<=p->bytes);invalidations++; }
static void flush(const nvn_obj_t *p,ptrdiff_t offset,size_t bytes)
{ assert(waited && !offset && bytes<=p->bytes);((nvn_obj_t *)p)->flushed=true;flushes++; }
static void queue_flush(nvn_queue_t *q) { (void)q;queue_flushed=true; }
static void *real_getproc(void *d,const char *name)
{
  assert(!d);
  if (!strcmp(name,"nvnQueueFlush")) return have_queue_flush?(void *)queue_flush:NULL;
  assert(!strcmp(name,"nvnMemoryPoolFlushMappedRange"));return have_flush?(void *)flush:NULL;
}
static struct {
  size_t (*cmd_free)(const nvn_obj_t *);
  int (*tex_width)(const nvn_texture_t *);
  void (*cmd_add_command)(nvn_obj_t *,const nvn_obj_t *,ptrdiff_t,size_t);
  void (*cmd_add_control)(nvn_obj_t *,void *,size_t);
  void (*cmd_begin)(nvn_obj_t *);
  void (*cmd_copy_to_buffer)(nvn_obj_t *,const nvn_texture_t *,const void *,const nvn_region_t *,nvn_addr_t,int);
  nvn_addr_t (*pool_address)(const nvn_obj_t *);
  nvn_handle_t (*cmd_end)(nvn_obj_t *);
  void (*queue_submit)(nvn_queue_t *,int,const nvn_handle_t *);
  void (*queue_fence)(nvn_queue_t *,nvn_obj_t *,int,int);
  void (*pool_invalidate)(const nvn_obj_t *,ptrdiff_t,size_t);
  void (*cmd_copy_to_texture)(nvn_obj_t *,nvn_addr_t,const nvn_texture_t *,const void *,const nvn_region_t *,int);
} fn={command_free,width,add_command,add_control,begin,to_buffer,address,end,submit,fence,invalidate,to_texture};
#include "../obj/tests/void_fill_production.inc"
static nvn_texture_t texture,original;
static void reset(void)
{
  free(world_reserved?world_reserved:(void *)world_pixels);world_reserved=NULL;world_pixels=NULL;world_capacity=0;world_retry=0;world_warned=false;
  queued_count=0;recording_slot=pending_slot=0;
  fail_pool=fail_alloc=failed=pending=false;void_fill_flush=NULL;have_flush=wait_ok=true;
  void_fill_queue_flush=NULL;have_queue_flush=true;
  swapchain_format=37;invalidations=flushes=uploads=waits=fail_wait=0;
  allocations=0;peak_bytes=0;read_bytes=0;read_commands=0;command_limit=CMD_SIZE;max_allocation=allocation_limit=2*1024*1024;
}
/* Present drains the final upload normally; production does not wait for it. */
static void present_world(void)
{
  world_commands_begin(0);assert(copy_world_fill(&texture));
  assert(world_commands_finish(NULL,0));
}
static void check_view(nvn_rect_t area)
{
  for (int y=0;y<texture.h;y++) for (int x=0;x<texture.w;x++)
    texture.pixels[y][x]=0x7f000000u|((x*123+y*789+waits*55555)&0xffffff);
  original=texture;
  assert(draw_world_fill(NULL,&texture,area,0));present_world();
  assert(invalidations && invalidations==flushes && !pending);
  assert(waits==invalidations*2 && peak_bytes<=allocation_limit);
  /* Compare every pixel against the independent band raster; outside stays put. */
  static uint8_t band[4096*64];
  for (int y=0;y<area.h;y+=64) {
    int h=area.h-y<64?area.h-y:64;
    void_view_band(&world_view,area.w,area.h,y,h,band);
    for (int yy=0;yy<h;yy++) for (int x=0;x<area.w;x++) {
      uint32_t expected=void_view_color(original.pixels[area.y+y+yy][area.x+x],band[yy*area.w+x]);
      assert(texture.pixels[area.y+y+yy][area.x+x]==expected);
    }
  }
  for (int y=0;y<texture.h;y++) for (int x=0;x<texture.w;x++)
    if (x<area.x || x>=area.x+area.w || y<area.y || y>=area.y+area.h)
      assert(texture.pixels[y][x]==original.pixels[y][x]);
}
int main(void)
{
  /* Nonzero crop, odd size, last partial band, holes and disconnected coverage. */
  nvn_rect_t area={17,11,641,367};texture.w=700;texture.h=400;
  for (int y=0;y<VOID_VIEW_HEIGHT;y++) for (int x=0;x<VOID_VIEW_WIDTH;x++)
    world_view.mask[y*VOID_VIEW_WIDTH+x]=(y%3!=0 && x%7<3);
  for (int frame=0;frame<2;frame++) {
    for (int y=0;y<400;y++) for (int x=0;x<700;x++)
      texture.pixels[y][x]=0x7f000000u|((x*123+y*789+frame*55555)&0xffffff);
    original=texture;
    assert(draw_world_fill(NULL,&texture,area,0));present_world();
    assert(invalidations && invalidations==flushes && !pending);
    for (int y=0;y<400;y++) for (int x=0;x<700;x++) {
      uint32_t expected=original.pixels[y][x];
      if (x>=area.x && x<area.x+area.w && y>=area.y && y<area.y+area.h &&
          world_view.mask[(y-area.y)*VOID_VIEW_HEIGHT/area.h*VOID_VIEW_WIDTH+(x-area.x)*VOID_VIEW_WIDTH/area.w])
        expected=void_view_blend(expected);
      assert(texture.pixels[y][x]==expected);
    }
  }
  /* Lines alone must allocate/read/copy their bands, with one-pixel thickness,
   * 75% alpha, and no tint across the reserved player region. */
  memset(&world_view,0,sizeof(world_view));world_view.count=2;
  world_view.lines[0]=(view_line_t){0,.5f,1,.5f,VIEW_WALL};
  world_view.lines[1]=(view_line_t){.5f,0,.5f,1,VIEW_WALL};
  world_view.hide_link=false;
  texture=original;
  assert(draw_world_fill(NULL,&texture,area,0));present_world();
  for (int y=0;y<400;y++) for (int x=0;x<700;x++) {
    uint32_t expected=original.pixels[y][x];int xx=x-area.x,yy=y-area.y;
    bool inside=xx>=0 && xx<area.w && yy>=0 && yy<area.h;
    if (inside && (xx==(area.w-1)/2 || yy==(area.h-1)/2)) expected=void_view_color(expected,VIEW_WALL);
    assert(texture.pixels[y][x]==expected);
  }
  world_view.hide_link=true;
  world_view.link[0]=world_view.link[2]=.5f;world_view.link[1]=.65f;world_view.link[3]=.35f;
  check_view(area);
  for (int failure=0;failure<6;failure++) {
    reset();
    if (failure==0) swapchain_format=61;
    if (failure==1) have_flush=false;
    if (failure==2) fail_alloc=true;
    if (failure==3) fail_pool=true;
    if (failure==4) wait_ok=false;
    if (failure==5) have_queue_flush=false;
    assert(!draw_world_fill(NULL,&texture,area,0));
    assert(!invalidations && !flushes && !uploads);
    assert(failure==4?failed:world_retry>0);
  }
  /* Regression: full 1440p coverage previously kept 23 separate 640 KiB
   * pools and went permanently blank when the eighth pool failed. */
  reset();memset(&world_view,0,sizeof(world_view));
  memset(world_view.mask,VIEW_VOID,sizeof(world_view.mask));
  texture.w=2560;texture.h=1440;check_view((nvn_rect_t){0,0,2560,1440});
  assert(allocations==1 && world_capacity==2*1024*1024 && waits>2);
  /* A larger viewport uses the same buffer, without restart/reallocation. */
  texture.w=3840;texture.h=2160;check_view((nvn_rect_t){0,0,3840,2160});
  assert(allocations==1);
  /* Fragmented heap: fall back to just 64 KiB, keeping every pixel intact. */
  reset();max_allocation=allocation_limit=64*1024;
  texture.w=2560;texture.h=1440;check_view((nvn_rect_t){0,0,2560,1440});
  assert(world_capacity==64*1024 && allocations==6);
  /* Sparse strips also flush safely when the metadata array is full. */
  memset(&world_view,0,sizeof(world_view));world_view.count=1;
  world_view.lines[0]=(view_line_t){.5f,0,.5f,1,VIEW_WALL};
  texture.w=4096;texture.h=4096;check_view((nvn_rect_t){0,0,4096,4096});
  /* A 1440p wire grid used to copy all 23 full-width strips (14.375 MiB)
   * and synchronously wait 16 times. Sparse packing should need one read wait. */
  reset();memset(&world_view,0,sizeof(world_view));
  for (int i=0;i<24;i++) world_view.lines[world_view.count++]=(view_line_t){0,(i+.5f)/24,1,(i+.5f)/24,VIEW_FLOOR};
  for (int i=0;i<32;i++) world_view.lines[world_view.count++]=(view_line_t){(i+.5f)/32,0,(i+.5f)/32,1,VIEW_WALL};
  texture.w=2560;texture.h=1440;check_view((nvn_rect_t){0,0,2560,1440});
  assert(invalidations==1 && waits==2 && read_bytes<2*1024*1024);
  printf("1440p wire grid: %u KiB copied, %u regions, one blocking readback wait (old: 14720 KiB, 16 waits)\n",
         (unsigned)(read_bytes/1024),read_commands);
  /* More than one plan array: completing a batch must not overwrite the band
   * still being scanned. Many tiny diagonal segments frustrate strip merging. */
  reset();memset(&world_view,0,sizeof(world_view));
  for (unsigned i=0;i<14000;i++) {
    unsigned x=(i%125)*32+(i/125)%7,y=(i/125)*32;
    world_view.lines[world_view.count++]=(view_line_t){x/4095.f,y/4095.f,(x+12)/4095.f,(y+11)/4095.f,VIEW_WALL};
  }
  texture.w=4096;texture.h=4096;check_view((nvn_rect_t){0,0,4096,4096});
  assert(read_commands>WORLD_PLANS);
  /* Memory pressure is retryable, and failed pools release their allocation. */
  for (int pool=0;pool<2;pool++) {
    reset();fail_alloc=!pool;fail_pool=pool;
    assert(!draw_world_fill(NULL,&texture,area,0) && !live_bytes && world_retry==120);
    fail_alloc=fail_pool=false;
    for (int frame=0;frame<120;frame++) assert(!draw_world_fill(NULL,&texture,area,0));
    check_view(area);assert(world_pixels);
  }
  /* Startup reservation keeps the full buffer even after the general heap
   * can only provide 128 KiB. No late allocation is needed. */
  reset();world_reserved=test_alloc(PAGE_SIZE,WORLD_BUFFER_MAX);
  max_allocation=128*1024;
  memset(&world_view,0,sizeof(world_view));world_view.count=1;
  world_view.lines[0]=(view_line_t){0,.5f,1,.5f,VIEW_WALL};
  check_view(area);assert(world_capacity==WORLD_BUFFER_MAX && allocations==1);
  reset();command_limit=1024;
  assert(!draw_world_fill(NULL,&texture,area,0) && failed && !pending && !invalidations);
  /* Leave frame one's final upload in flight, then read frame two into the
   * same scratch pool using the other command slot. Queue order protects both. */
  reset();memset(&world_view,0,sizeof(world_view));world_view.count=1;
  world_view.lines[0]=(view_line_t){0,.5f,1,.5f,VIEW_WALL};
  texture.w=original.w=700;texture.h=original.h=400;
  for (int y=0;y<400;y++) for (int x=0;x<700;x++) {
    texture.pixels[y][x]=0xff222222u;original.pixels[y][x]=0xffeeeeeeu;
  }
  assert(draw_world_fill(NULL,&texture,area,0));
  world_commands_begin(0);assert(copy_world_fill(&texture));
  nvn_handle_t queued_upload=end(&cmd_buffer);submit(NULL,1,&queued_upload);
  fence(NULL,slot_sync,SYNC_ALL_GPU_COMMANDS_COMPLETE,0);queue_flush(NULL);
  assert(pending && texture.pixels[194][100]==0xff222222u);
  assert(draw_world_fill(NULL,&original,area,1));
  assert(texture.pixels[194][100]==void_view_color(0xff222222u,VIEW_WALL));
  world_commands_begin(1);assert(copy_world_fill(&original));assert(world_commands_finish(NULL,1));
  assert(original.pixels[194][100]==void_view_color(0xffeeeeeeu,VIEW_WALL));
  /* A stalled upload must not allow scratch/command storage to be reused. */
  reset();fail_wait=2;memset(world_view.mask,VIEW_VOID,sizeof(world_view.mask));
  max_allocation=allocation_limit=64*1024;
  assert(!draw_world_fill(NULL,&texture,area,0) && failed && pending && waits==2);
  reset();
  puts("PASS: bounded NVN batches at 1440p/4K, 64 KiB fallback/recovery, resize, deferred read/upload fences, cache ordering, alpha/crop/player masking");
}
