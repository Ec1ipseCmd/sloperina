#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ootgz/collision.c"
static uint64_t objects[19][2560],original[19][2560];
#define A(i) ((uintptr_t)objects[i])
uintptr_t z64hd_main=0x100000000ull;
bool voids_shown,airwalls_shown;
static bool loading,changing,mapped=true;
static uintptr_t actor=123;
static unsigned drawn;
static float farthest;
bool sys_mapped(uintptr_t at,size_t n,uint32_t perm)
{
  assert(perm==SYS_PERM_R);
  if (!mapped || n>UINTPTR_MAX-at) return false;
  for (unsigned i=0;i<19;i++) if (at>=A(i)&&at+n<=A(i)+sizeof(objects[i])) return true;
  return false;
}
uintptr_t sys_ptr(uintptr_t at)
{
  if (at==MAIN(Z64HD_SCENE_MGR_PTR)) return A(0);
  uintptr_t v=0;if (sys_mapped(at,8,SYS_PERM_R)) memcpy(&v,(void *)at,8);return v;
}
bool sys_read32(uintptr_t at,uint32_t *v)
{ if (!sys_mapped(at,4,SYS_PERM_R)) return false;memcpy(v,(void *)at,4);return true; }
bool gd_get_bool(uint32_t hash,bool *out)
{ *out=hash==0xEF2B60C2u?loading:changing;return true; }
bool link_drawn_pos(vec3f_t *p) { *p=(vec3f_t){0,0,0};return true; }
uintptr_t link_actor(void) { return actor; }
bool actors_each(void (*visit)(uintptr_t,const char *,void *),void *ctx)
{ (void)visit;(void)ctx;return true; }
void collision_shape_scan(const void *bytes,size_t size,const float world[3][4],float scale,collision_scan_t *s)
{ (void)bytes;(void)size;(void)world;(void)scale;(void)s;assert(false); }
void collision_mesh_scan(const void *bytes,size_t size,collision_scan_t *s)
{
  assert(bytes==(void *)A(6)&&size==0x1000);
  for (size_t i=s->skip;i<20000 && s->remaining;i++) {
    s->skip=0;s->visited++;s->remaining--;
    float x=(float)(i%100)*.1f-5,y=(float)((i/100)%100)*.1f-5;
    vec3f_t v[3]={{x,y,3},{x+.11f,y,3},{x,y+.11f,3}};
    s->triangle(v,COLLISION_WALL,s->ctx);
  }
}
static void put(uintptr_t at,uintptr_t v) { memcpy((void *)at,&v,8); }
static void test_actor_sources(void)
{
  put(A(7)+0x270,A(8));put(A(8)+0x50,A(9));put(A(9)+0x20,A(10));
  put(A(10)+0x18,1);put(A(10)+0x20,A(11));put(A(11),A(12));put(A(12)+8,A(13));
  put(A(13)+0x10,1);put(A(13)+0x18,A(11)+0x100);put(A(11)+0x108,A(14));
  put(A(14),MAIN(Z64HD_VT_ENTITY_BODY));put(A(14)+0x28,A(15));
  put(A(15),MAIN(Z64HD_VT_MESH_WRAPPER));put(A(15)+0xA0,A(16)+0x48);
  put(A(16),MAIN(Z64HD_VT_SHAPE_DESCRIPTOR));put(A(16)+8,A(17));put(A(16)+0x10,0x1000);
  put(A(16)+0x48,MAIN(Z64HD_VT_SHAPE_RESOURCE));
  put(A(15)+0x80,A(18));put(A(18)+8,A(17)+0x50);put(A(17)+0x80,A(16)+0x48);
  memcpy(original,objects,sizeof(original));
  unsigned used=TILE_COUNT;
  actor_shapes(A(7),"scenery",&used);
  assert(used==TILE_COUNT+1 && current[TILE_COUNT].data==A(17) && current[TILE_COUNT].body==A(14));
  assert(!memcmp(original,objects,sizeof(original)));
  for (int invalid=0;invalid<4;invalid++) {
    memcpy(objects,original,sizeof(objects));used=TILE_COUNT;
    if (invalid==0) put(A(16),0); /* independently allocated resource, not inline */
    if (invalid==1) put(A(18)+8,A(17)+0x2000); /* root outside file */
    if (invalid==2) put(A(13)+0x10,129);
    if (invalid==3) put(A(14),0);
    actor_shapes(A(7),"scenery",&used);assert(used==TILE_COUNT);
  }
  memset(objects,0,sizeof(objects));
  puts("PASS: live actor shape ownership, file/root bounds, count/type guards and read-only collection");
}
static void draw(const vec3f_t v[3],int kind,void *ctx)
{
  (void)ctx;assert(kind==COLLISION_WALL && v[0].z==3);drawn++;
  for (int i=0;i<3;i++) if (v[i].x>farthest) farthest=v[i].x;
}
static void finish(void)
{ unsigned frames=0;do { collision_frame();assert(++frames<8); } while (collecting); }
int main(void)
{
  test_actor_sources();
  put(A(0)+0x1F0,A(1));put(A(1)+0x58,A(2));put(A(2)+0x50,A(3));
  put(A(3),MAIN(Z64HD_VT_STATIC_MGR));put(A(3)+0x720,A(4));
  put(A(4),MAIN(Z64HD_VT_STATIC_BODY));put(A(4)+0x10,A(5));
  put(A(5),MAIN(Z64HD_VT_STATIC_RESOURCE));put(A(5)+8,A(6));put(A(5)+0x10,0x1000);
  collision_shown=true;
  memcpy(original,objects,sizeof(original));
  collision_frame();assert(collecting&&cursor==16384&&!visible_count);
  collision_frame();assert(!collecting&&ready&&visible_count>0&&visible_count<TRIANGLE_MAX);
  collision_each_triangle(draw,NULL);assert(drawn==visible_count && farthest>=4.5f);
  triangle_t snapshot[TRIANGLE_MAX];memcpy(snapshot,visible,sizeof(snapshot));
  unsigned saved_count=visible_count;
  for (int i=0;i<1;i++) {
    collision_frame();assert(collecting && visible_count==saved_count);
    assert(!memcmp(snapshot,visible,sizeof(snapshot))); /* no partial replacements */
  }
  collision_frame();assert(!collecting && visible_count==saved_count);
  assert(!memcmp(snapshot,visible,sizeof(snapshot))); /* stable stationary scan */
  assert(!memcmp(original,objects,sizeof(original)));
  loading=true;collision_frame();assert(!visible_count&&!collecting);
  loading=false;finish();assert(visible_count);
  changing=true;collision_frame();assert(!visible_count);
  changing=false;finish();assert(visible_count);
  actor++;collision_frame();assert(!visible_count);finish();assert(visible_count);
  collision_kinds=2;collision_frame();assert(!visible_count);
  finish();assert(visible_count);
  collision_detail=0;collision_frame();assert(!visible_count);finish();assert(visible_count==TRIANGLE_MAX);
  collision_detail=3;finish();assert(visible_count<TRIANGLE_MAX);
  put(A(3)+0x720,0);collision_frame();assert(!visible_count);
  put(A(3)+0x720,A(4));finish();assert(visible_count);
  mapped=false;collision_frame();assert(!visible_count);
  mapped=true;finish();assert(visible_count);
  collision_shown=false;collision_frame();assert(!visible_count);
  /* Void occlusion collects exact solids independently of every display filter. */
  voids_shown=true;collision_kinds=0;collision_radius=1;collision_detail=3;
  collision_frame();assert(collecting && !occluder_count && !collision_occlusion_range((vec3f_t){0}));
  finish();assert(occluder_count==20000 && !visible_count);
  assert(collision_occlusion_range((vec3f_t){0})>63);
  assert(collision_occlusion_range((vec3f_t){100,0,0})==0);
  assert(occluders[0].v[1].x-occluders[0].v[0].x>.10f && occluders[0].v[1].x-occluders[0].v[0].x<.12f);
  collision_frame();assert(collecting && occluder_count==20000);
  loading=true;collision_frame();assert(!occluder_count && !collision_occlusion_range((vec3f_t){0}));
  loading=false;finish();assert(occluder_count==20000);
  collision_shown=true;collision_frame();assert(!occluder_count);finish();assert(occluder_count);
  /* At capacity retain all nearer AABBs; a trigger never occupies a blocker slot. */
  occluder_candidate_count=0;
  for (unsigned i=0;i<OCCLUDER_MAX+100;i++) {
    float z=1+(float)i*.001f;
    vec3f_t v[3]={{0,0,z},{.1f,0,z},{0,.1f,z}};collect(v,COLLISION_WALL,NULL);
  }
  assert(occluder_candidate_count==OCCLUDER_MAX);
  float far=1+(OCCLUDER_MAX-1)*.001f;
  assert(occluder_candidates[0].distance>=far*far-.01f && occluder_candidates[0].distance<=far*far+.01f);
  vec3f_t near_trigger[3]={{0,0,1},{1,0,1},{0,1,1}};
  collect(near_trigger,COLLISION_TRIGGER,NULL);
  assert(occluder_candidates[0].distance>=far*far-.01f);
  voids_shown=false;collision_shown=false;collision_frame();assert(!occluder_count);
  airwalls_shown=true;finish();assert(occluder_count && !visible_count);
  assert(collision_occlusion_range((vec3f_t){0})>63);
  airwalls_shown=false;collision_frame();assert(!occluder_count);
  assert(!memcmp(original,objects,sizeof(original)));
  memset(seen,0,sizeof(seen));
  triangle_t trigger={.v={{.1f,.1f,3},{.2f,.1f,3},{.1f,.2f,3}},.kind=COLLISION_TRIGGER};
  assert(preview_triangle(&trigger) && trigger.v[0].x==.1f && trigger.v[2].x==.2f);
  assert(!preview_triangle(&trigger));
  puts("PASS: complete stable snapshots, dense-mesh coverage, exact mode, unsnapped triggers, bounded selection, loads, replacements, filters and read-only access");
}
