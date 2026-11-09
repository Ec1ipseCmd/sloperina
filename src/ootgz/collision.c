#include "collision.h"
#include "gamedata.h"
#include "sys.h"
#include "z64hd.h"

bool collision_shown;
int collision_kinds = 7; /* solid floors, walls, ceilings; triggers opt in */
int collision_radius = 12;
int collision_detail = 3; /* exact, quarter, half, one world unit */

#define TILE_COUNT 17
#define TRIANGLE_MAX 2048
#define SEEN_MAX 16384
#define SCAN_BUDGET 4096
typedef struct { uintptr_t data; uint32_t size; } tile_t;
typedef struct { vec3f_t v[3]; float distance; int kind; } triangle_t;
static tile_t tiles[TILE_COUNT];
static triangle_t candidates[TRIANGLE_MAX], visible[TRIANGLE_MAX];
static struct { vec3f_t v[3]; int kind; } seen[SEEN_MAX];
static unsigned count, visible_count, tile_count;
static size_t cursor;
static bool collecting, ready;
static unsigned unsupported;
static vec3f_t centre;
static int previous_kinds, previous_radius, previous_detail;
static uintptr_t previous_actor;

static uintptr_t next(uintptr_t p, unsigned off)
{ return p && !(p&7) && p<=UINTPTR_MAX-off ? sys_ptr(p+off) : 0; }

static void reset(void)
{
  count=visible_count=0; cursor=0; collecting=false; ready=false;
}

/* A fixed 4x4 streaming tile window plus the map-wide tile. These are native
 * StaticCompoundBodyMgr slots, not a heap scan. Check every owner/type again. */
static bool loaded_tiles(tile_t out[TILE_COUNT])
{
  uintptr_t scene=next(sys_ptr(MAIN(Z64HD_SCENE_MGR_PTR)),0x1F0);
  uintptr_t components=next(scene,0x58),mgr=next(components,0x50);
  if (next(mgr,0)!=MAIN(Z64HD_VT_STATIC_MGR)) return false;
  for (unsigned i=0;i<TILE_COUNT;i++) {
    uintptr_t slot=mgr+(i==16?0x6F0:0x38+i*0x48);
    uintptr_t body=next(slot,0x30),res=next(body,0x10);
    if (next(body,0)!=MAIN(Z64HD_VT_STATIC_BODY)||next(res,0)!=MAIN(Z64HD_VT_STATIC_RESOURCE)) continue;
    uintptr_t data=next(res,8); uint32_t size;
    if (!data || !sys_read32(res+0x10,&size) || size<0x510 || size>(128u<<20) ||
        data>UINTPTR_MAX-size || !sys_mapped(data,size,SYS_PERM_R)) continue;
    if (next(slot,0x30)==body && next(body,0x10)==res && next(res,8)==data) out[i]=(tile_t){data,size};
  }
  return true;
}

static bool point_before(vec3f_t a,vec3f_t b)
{ return a.x!=b.x?a.x<b.x:a.y!=b.y?a.y<b.y:a.z<b.z; }

/* Snap to a fixed world grid, discard collapsed faces and merge duplicates.
 * This is a display approximation only; triggers retain their exact bounds. */
static bool preview_triangle(triangle_t *item)
{
  static const float grids[]={0,.25f,.5f,1};
  float grid=collision_detail>=0 && collision_detail<4?grids[collision_detail]:1;
  if (grid && item->kind!=COLLISION_TRIGGER) {
    for (int i=0;i<3;i++) {
      float p[3]={item->v[i].x,item->v[i].y,item->v[i].z};
      for (int j=0;j<3;j++) {
        float scaled=p[j]/grid;
        p[j]=(float)(int)(scaled+(scaled<0?-.5f:.5f))*grid;
      }
      item->v[i]=(vec3f_t){p[0],p[1],p[2]};
    }
  }
  vec3f_t a=item->v[0],b=item->v[1],c=item->v[2];
  float x=(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y);
  float y=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z);
  float z=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
  if (x*x+y*y+z*z<.00000001f) return false;
  /* Canonical vertex order makes winding and shared instances immaterial. */
  for (int i=1;i<3;i++) {
    vec3f_t v=item->v[i]; int at=i;
    while (at && point_before(v,item->v[at-1])) { item->v[at]=item->v[at-1];at--; }
    item->v[at]=v;
  }
  uint32_t words[9],hash=2166136261u;
  memcpy(words,item->v,sizeof(words));
  for (int i=0;i<9;i++) {
    if ((words[i]&0x7fffffffu)==0) words[i]=0; /* negative zero */
    hash=(hash^words[i])*16777619u;
  }
  hash=(hash^(unsigned)item->kind)*16777619u;
  /* Bounded probes even when the detailed preview fills the table. */
  for (unsigned probe=0;probe<64;probe++) {
    unsigned at=(hash+probe)&(SEEN_MAX-1);
    if (!seen[at].kind) {
      memcpy(seen[at].v,item->v,sizeof(item->v));seen[at].kind=item->kind+1;
      return true;
    }
    bool equal=seen[at].kind==item->kind+1;
    for (int i=0;i<3 && equal;i++) {
      vec3f_t p=seen[at].v[i],q=item->v[i];
      equal=p.x==q.x && p.y==q.y && p.z==q.z;
    }
    if (equal) return false;
  }
  return true;
}

/* Nearest unique preview triangles in a bounded max heap. */
static void collect(const vec3f_t v[3], int kind, void *ctx)
{
  (void)ctx;
  float low[3]={v[0].x,v[0].y,v[0].z},high[3]={v[0].x,v[0].y,v[0].z};
  for (int i=1;i<3;i++) {
    float p[3]={v[i].x,v[i].y,v[i].z};
    for (int j=0;j<3;j++) { if (p[j]<low[j]) low[j]=p[j]; if (p[j]>high[j]) high[j]=p[j]; }
  }
  float p[3]={centre.x,centre.y,centre.z},distance=0;
  for (int j=0;j<3;j++) { float d=p[j]<low[j]?low[j]-p[j]:p[j]>high[j]?p[j]-high[j]:0; distance+=d*d; }
  if (distance>(float)(collision_radius*collision_radius)) return;
  triangle_t item={ .distance=distance,.kind=kind };
  memcpy(item.v,v,sizeof(item.v));
  if (!preview_triangle(&item)) return;
  if (count<TRIANGLE_MAX) {
    unsigned at=count++;
    while (at && candidates[(at-1)/2].distance<distance) { candidates[at]=candidates[(at-1)/2]; at=(at-1)/2; }
    candidates[at]=item;
  } else if (distance<candidates[0].distance) {
    unsigned at=0;
    while (at*2+1<count) {
      unsigned child=at*2+1;
      if (child+1<count && candidates[child+1].distance>candidates[child].distance) child++;
      if (candidates[child].distance<=distance) break;
      candidates[at]=candidates[child]; at=child;
    }
    candidates[at]=item;
  }
}

void collision_frame(void)
{
  bool loading=false,changing=false;
  gd_get_bool(0xEF2B60C2u,&loading);
  gd_get_bool(0xF8633C7Fu,&changing);
  vec3f_t player;
  if (!collision_shown || !collision_kinds || loading || changing || !link_drawn_pos(&player) ||
      !(player.x>-100000 && player.x<100000 && player.y>-100000 && player.y<100000 &&
        player.z>-100000 && player.z<100000)) { tile_count=0; reset(); return; }
  tile_t current[TILE_COUNT]={0};
  if (!loaded_tiles(current)) { tile_count=0; reset(); return; }
  uintptr_t actor=link_actor();
  bool changed=actor!=previous_actor || previous_kinds!=collision_kinds ||
               previous_radius!=collision_radius || previous_detail!=collision_detail;
  tile_count=0;
  for (unsigned i=0;i<TILE_COUNT;i++) {
    if (current[i].data!=tiles[i].data || current[i].size!=tiles[i].size) changed=true;
    if (current[i].data) tile_count++;
    tiles[i]=current[i];
  }
  previous_actor=actor; previous_kinds=collision_kinds; previous_radius=collision_radius;
  previous_detail=collision_detail;
  if (changed) reset();
  float dx=player.x-centre.x,dy=player.y-centre.y,dz=player.z-centre.z;
  if (dx*dx+dy*dy+dz*dz>64) reset(); /* teleport; old geometry must disappear */
  if (!collecting) {
    centre=player; count=0; cursor=0; collecting=true;
    memset(seen,0,sizeof(seen));
  }
  collision_scan_t scan={ .skip=cursor,.remaining=SCAN_BUDGET,.kinds=(unsigned)collision_kinds,
                         .main=z64hd_main,.triangle=collect };
  for (unsigned i=0;i<TILE_COUNT && scan.remaining;i++)
    if (tiles[i].data) collision_mesh_scan((const void *)tiles[i].data,tiles[i].size,&scan);
  cursor+=scan.visited;
  /* Retain the complete old snapshot throughout the next scan. Publishing a
   * partial heap here made most geometry disappear every few frames. */
  if (scan.remaining) {
    visible_count=count;
    memcpy(visible,candidates,sizeof(triangle_t)*count);
    for (unsigned i=1;i<count;i++) {
      triangle_t v=visible[i]; unsigned at=i;
      while (at && visible[at-1].distance>v.distance) { visible[at]=visible[at-1]; at--; }
      visible[at]=v;
    }
    ready=true;
  }
  if (scan.remaining) { collecting=false; unsupported=scan.unsupported; }
}

void collision_each_triangle(void (*draw)(const vec3f_t [3],int,void *),void *ctx)
{ for (unsigned i=0;i<visible_count;i++) draw(visible[i].v,visible[i].kind,ctx); }

void collision_summary(char *buf,int len)
{
  if (!collision_shown) snprintf(buf,len,"enable show surfaces to inspect");
  else if (!collision_kinds) snprintf(buf,len,"select a surface or trigger type");
  else if (!tile_count) snprintf(buf,len,"loaded collision: N/A");
  else snprintf(buf,len,"%u triangles, %u tiles%s%s",visible_count,tile_count,
                ready?"":" (scanning)",unsupported?"; some shapes omitted":"");
}
