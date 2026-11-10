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
typedef struct { uint32_t pixels[400][700]; } nvn_texture_t;
typedef struct { int x,y,z,w,h,d; } nvn_region_t;
typedef struct { int x,y,w,h; } nvn_rect_t;
typedef uintptr_t nvn_addr_t;
typedef uint64_t nvn_handle_t;
#define PAGE_SIZE 4096
#define CMD_SIZE 8192
#define CTRL_SIZE 8192
#define POOL_CPU_CACHED 4
#define POOL_GPU_UNCACHED 16
#define SYNC_ALL_GPU_COMMANDS_COMPLETE 0
static nvn_obj_t cmd_buffer,command_pool,slot_sync[2];
static char control_mem[CTRL_SIZE*2];
static bool slot_used[2],wait_ok=true,waited,failed,fail_pool,fail_alloc,have_flush=true;
static bool queue_flushed,have_queue_flush=true;
static int swapchain_format=37,invalidations,flushes,uploads;
static void *device;
static size_t round_page(size_t n) { return (n+4095)&~(size_t)4095; }
static bool is_rgba8(int n) { return n==37; }
static void log_printf(const char *fmt,...) { (void)fmt; }
static void overlay_fail(const char *message) { (void)message;failed=true; }
static void *test_alloc(size_t alignment,size_t size)
{ assert(alignment==PAGE_SIZE && !(size%PAGE_SIZE));return fail_alloc?NULL:malloc(size); }
#define aligned_alloc test_alloc
static bool make_pool(nvn_obj_t *p,void *data,size_t size,int flags)
{ assert(flags==(POOL_CPU_CACHED|POOL_GPU_UNCACHED));*p=(nvn_obj_t){data,size,false};return !fail_pool; }
static struct { nvn_texture_t *tex;nvn_obj_t *pool;nvn_region_t r;bool upload; } ops[64];
static unsigned operations;
static void add_command(nvn_obj_t *b,const nvn_obj_t *p,ptrdiff_t at,size_t n)
{ (void)b;(void)p;assert(at==0 && n==CMD_SIZE); }
static void add_control(nvn_obj_t *b,void *p,size_t n)
{ (void)b;assert(p==control_mem && n==CTRL_SIZE); }
static void begin(nvn_obj_t *b) { (void)b;operations=0;waited=false;queue_flushed=false; }
static nvn_handle_t end(nvn_obj_t *b) { (void)b;return operations; }
static nvn_addr_t address(const nvn_obj_t *p) { return (uintptr_t)p; }
static int width(const nvn_texture_t *t) { (void)t;return 700; }
static void record(nvn_texture_t *t,nvn_obj_t *p,const nvn_region_t *r,bool upload)
{
  assert(operations<64 && r->x>=0 && r->y>=0 && r->x+r->w<=700 && r->y+r->h<=400);
  assert(r->z==0 && r->d==1 && (size_t)r->w*r->h*4<=p->bytes);
  if (upload) { assert(p->flushed);uploads++; }
  ops[operations].tex=t;ops[operations].pool=p;ops[operations].r=*r;
  ops[operations++].upload=upload;
}
static void to_buffer(nvn_obj_t *b,const nvn_texture_t *t,const void *v,const nvn_region_t *r,nvn_addr_t a,int flags)
{ (void)b;assert(!v && !flags);record((nvn_texture_t *)t,(nvn_obj_t *)a,r,false); }
static void to_texture(nvn_obj_t *b,nvn_addr_t a,const nvn_texture_t *t,const void *v,const nvn_region_t *r,int flags)
{ (void)b;assert(!v && !flags);record((nvn_texture_t *)t,(nvn_obj_t *)a,r,true); }
static void submit(nvn_queue_t *q,int n,const nvn_handle_t *h)
{ (void)q;assert(n==1 && *h==operations); }
static void fence(nvn_queue_t *q,nvn_obj_t *sync,int condition,int flags)
{ (void)q;assert(sync==slot_sync && condition==0 && !flags); }
static bool slot_finished(int slot)
{
  assert(slot==0 && slot_used[slot] && queue_flushed);
  if (!wait_ok) return false;
  for (unsigned i=0;i<operations;i++) {
    nvn_region_t r=ops[i].r;nvn_obj_t *p=ops[i].pool;nvn_texture_t *t=ops[i].tex;
    for (int y=0;y<r.h;y++) for (int x=0;x<r.w;x++) {
      if (ops[i].upload) t->pixels[y+r.y][x+r.x]=p->pixels[y*r.w+x];
      else p->pixels[y*r.w+x]=t->pixels[y+r.y][x+r.x];
    }
    if (!ops[i].upload) p->flushed=false;
  }
  waited=true;return true;
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
} fn={width,add_command,add_control,begin,to_buffer,address,end,submit,fence,invalidate,to_texture};
#include "../obj/tests/void_fill_production.inc"
static nvn_texture_t texture,original;
static void reset(void)
{
  for (int i=0;i<VOID_FILL_BANDS;i++) free(void_fill_bands[i].pixels);
  memset(void_fill_bands,0,sizeof(void_fill_bands));
  void_fill_failed=false;fail_pool=fail_alloc=false;void_fill_flush=NULL;have_flush=wait_ok=true;
  void_fill_queue_flush=NULL;have_queue_flush=true;
  swapchain_format=37;invalidations=flushes=uploads=0;
}
int main(void)
{
  /* Nonzero crop, odd size, last partial band, holes and disconnected coverage. */
  nvn_rect_t area={17,11,641,367};
  for (int y=0;y<VOID_VIEW_HEIGHT;y++) for (int x=0;x<VOID_VIEW_WIDTH;x++)
    world_view.mask[y*VOID_VIEW_WIDTH+x]=(y%3!=0 && x%7<3);
  for (int frame=0;frame<2;frame++) {
    for (int y=0;y<400;y++) for (int x=0;x<700;x++)
      texture.pixels[y][x]=0x7f000000u|((x*123+y*789+frame*55555)&0xffffff);
    original=texture;
    assert(prepare_void_fill(NULL,&texture,area,0));
    assert(invalidations && invalidations==flushes && !memcmp(&texture,&original,sizeof(texture)));
    begin(&cmd_buffer);copy_void_fill(&texture);
    nvn_handle_t h=end(&cmd_buffer);submit(NULL,1,&h);queue_flush(NULL);assert(slot_finished(0));
    for (int y=0;y<400;y++) for (int x=0;x<700;x++) {
      uint32_t expected=original.pixels[y][x];
      if (x>=area.x && x<area.x+area.w && y>=area.y && y<area.y+area.h &&
          world_view.mask[(y-area.y)*VOID_VIEW_HEIGHT/area.h*VOID_VIEW_WIDTH+(x-area.x)*VOID_VIEW_WIDTH/area.w])
        expected=void_view_blend(expected);
      assert(texture.pixels[y][x]==expected);
    }
  }
  /* Lines alone must allocate/read/copy their bands, with one-pixel thickness,
   * the same 50% alpha, and no tint across the reserved player region. */
  memset(&world_view,0,sizeof(world_view));world_view.count=2;
  world_view.lines[0]=(view_line_t){0,.5f,1,.5f,VIEW_WALL};
  world_view.lines[1]=(view_line_t){.5f,0,.5f,1,VIEW_WALL};
  world_view.hide_link=true;
  world_view.link[0]=world_view.link[1]=.4f;world_view.link[2]=world_view.link[3]=.6f;
  texture=original;
  assert(prepare_void_fill(NULL,&texture,area,0));begin(&cmd_buffer);copy_void_fill(&texture);
  nvn_handle_t handle=end(&cmd_buffer);submit(NULL,1,&handle);queue_flush(NULL);assert(slot_finished(0));
  for (int y=0;y<400;y++) for (int x=0;x<700;x++) {
    uint32_t expected=original.pixels[y][x];int xx=x-area.x,yy=y-area.y;
    bool inside=xx>=0 && xx<area.w && yy>=0 && yy<area.h;
    bool player=xx>=(int)(.4f*area.w)-1 && xx<=(int)(.6f*area.w)+1 &&
                yy>=(int)(.4f*area.h)-1 && yy<=(int)(.6f*area.h)+1;
    if (inside && !player && (xx==(area.w-1)/2 || yy==(area.h-1)/2)) expected=void_view_color(expected,VIEW_WALL);
    assert(texture.pixels[y][x]==expected);
  }
  for (int failure=0;failure<6;failure++) {
    reset();
    if (failure==0) swapchain_format=61;
    if (failure==1) have_flush=false;
    if (failure==2) fail_alloc=true;
    if (failure==3) fail_pool=true;
    if (failure==4) wait_ok=false;
    if (failure==5) have_queue_flush=false;
    assert(!prepare_void_fill(NULL,&texture,area,0));
    assert(!invalidations && !flushes && !uploads);
    assert(failure==4?failed:void_fill_failed);
  }
  reset();
  puts("PASS: production NVN fill, deferred readback/fence/invalidate/blend/flush/upload ordering, current frame, untouched pixels/crop, failure paths");
}
