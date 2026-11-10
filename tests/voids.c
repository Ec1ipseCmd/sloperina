#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "z64hd.h"
static int32_t original(uintptr_t, uintptr_t, const uint32_t *, const uint32_t *);
#undef MAIN_FN
#define MAIN_FN(type, off) ((type)original)
#include "../src/ootgz/voids.c"

static uint64_t objects[24][1024], before[24][1024];
#define A(i) ((uintptr_t)objects[i])
uintptr_t z64hd_main = 0x100000000ull;
static uintptr_t blocked, player_actor;
static bool floating, good_list = true;
static int calls, drawn;
static int notices, logs;
static bool active_box;
static vec3f_t drawn_corners[8];
static const uint32_t *forward_target, *forward_payload;
void gfx_notice(const char *text) { assert(!strcmp(text,"void-out blocked")); notices++; }
void log_printf(const char *fmt, ...) { assert(strstr(fmt,"void-out blocked:")); logs++; }

bool sys_mapped(uintptr_t a, size_t n, uint32_t perm)
{
  assert(perm == SYS_PERM_R);
  if (n > UINTPTR_MAX-a) return false;
  for (int i=0; i<24; i++)
    if (a>=A(i) && a+n<=A(i)+sizeof(objects[i]) && A(i)!=blocked) return true;
  return false;
}
uintptr_t sys_ptr(uintptr_t a) { uintptr_t p=0; if (sys_mapped(a,8,SYS_PERM_R)) memcpy(&p,(void *)a,8); return p; }
bool sys_read32(uintptr_t a,uint32_t *v) { if (!sys_mapped(a,4,SYS_PERM_R)) return false; memcpy(v,(void *)a,4); return true; }
uintptr_t link_actor(void) { return player_actor; }
bool levitate_active(void) { return floating; }
bool actors_each(void (*visit)(uintptr_t,const char *,void *),void *ctx)
{ visit(A(0),"Area",ctx); visit(A(0),"Enemy_Test",ctx); return good_list; }
static void ptr(uintptr_t a, uintptr_t p) { memcpy((void *)a,&p,8); }
static void integer(uintptr_t a,uint32_t v) { memcpy((void *)a,&v,4); }
static void real(uintptr_t a,float v) { memcpy((void *)a,&v,4); }
static int32_t original(uintptr_t sender,uintptr_t arg,const uint32_t *target,const uint32_t *payload)
{ assert(sender==123 && arg==456); calls++; forward_target=target; forward_payload=payload; return 17; }
static void draw(const vec3f_t corners[8],bool active,void *ctx)
{ assert(!ctx); drawn++; active_box=active; memcpy(drawn_corners,corners,sizeof(drawn_corners)); }
static void setup(void)
{
  memset(objects,0,sizeof(objects)); blocked=0; floating=false; good_list=true; calls=0;
  ptr(A(0)+0x270,A(1)); ptr(A(1)+0x50,A(2)); ptr(A(2)+0x20,A(3));
  integer(A(3)+0x38,1); ptr(A(3)+0x40,A(4)); ptr(A(4),A(22)); ptr(A(22)+8,A(5));
  integer(A(5)+0x10,1); ptr(A(5)+0x18,A(6)); ptr(A(6)+8,A(7));
  ptr(A(7),MAIN(Z64HD_VT_SENSOR_BODY)); ptr(A(7)+0x28,A(8));
  ptr(A(8),A(9)); ptr(A(9)+0x78,MAIN(Z64HD_SHAPE_CACHED_BOUNDS));
  real(A(8)+0x10,1); real(A(8)+0xC0,4.58f); real(A(8)+0xC4,4); real(A(8)+0xC8,5); real(A(8)+0xD4,4);
  real(A(7)+0x80,1); real(A(7)+0x94,1); real(A(7)+0xA8,1);
  real(A(7)+0x8C,-24); real(A(7)+0x9C,100); real(A(7)+0xAC,31);
  ptr(A(0)+0x3D8,A(10)); ptr(A(10)+0x80,A(11)); ptr(A(11),MAIN(Z64HD_VT_AREA_BINDER));
  ptr(A(11)+8,A(12)); ptr(A(12),MAIN(Z64HD_VT_AREA_LOGIC_NODE)); ptr(A(12)+8,A(13));
  integer(A(13)+0x60,1); ptr(A(13)+0x68,A(14)); ptr(A(14),A(15));
  ptr(A(15),MAIN(Z64HD_VT_ABYSS_NODE)); ptr(A(15)+8,A(13)); ptr(A(15)+0x28,A(12)+0x80);
  ptr(A(15)+0x78,A(16)); integer(A(16),8); ptr(A(15)+0x68,A(17)); ptr(A(17),0);
  ptr(A(15)+0x60,A(19)); integer(A(19),1);
  player_actor=A(21); ptr(player_actor,MAIN(Z64HD_VT_LINKOBJ)); integer(player_actor+0x3E8,42);
  voids_shown=true; voids_mode=VOIDS_NORMAL;
}
static int run(void)
{
  memcpy(before,objects,sizeof(objects)); voids_frame();
  assert(!memcmp(before,objects,sizeof(objects)));
  drawn=0; voids_each_box(draw,NULL); return drawn;
}
int main(void)
{
  void_nearest_t nearest;
  box_count=1;boxes[0].active=true;
  for (int i=0;i<8;i++) boxes[0].corners[i]=(vec3f_t){i&1?2:-2,i&2?3:-3,i&4?4:-4};
  assert(voids_nearest((vec3f_t){0,0,0},&nearest) && nearest.inside && nearest.distance==2);
  assert(voids_nearest((vec3f_t){5,0,0},&nearest) && !nearest.inside && nearest.distance==3 && nearest.point.x==2);
  assert(voids_nearest((vec3f_t){2,0,0},&nearest) && nearest.inside && nearest.distance==0);
  assert(voids_nearest((vec3f_t){5,7,4},&nearest) && nearest.distance==5);
  box_count=2;boxes[1]=boxes[0];
  for (int i=0;i<8;i++) boxes[1].corners[i].x+=2.1f;
  assert(voids_nearest((vec3f_t){0,0,0},&nearest) && nearest.inside && nearest.distance==2);
  box_count=1;
  /* Rotated/translated box: local x becomes world y, local y becomes -x. */
  for (int i=0;i<8;i++) {
    vec3f_t p=boxes[0].corners[i];boxes[0].corners[i]=(vec3f_t){10-p.y,20+p.x,30+p.z};
  }
  assert(voids_nearest((vec3f_t){10,25,30},&nearest) && nearest.distance==3 && nearest.point.y==22);
  boxes[0].active=false;assert(!voids_nearest((vec3f_t){10,20,30},&nearest));
  boxes[0].active=true;assert(!voids_nearest((vec3f_t){NAN,0,0},&nearest));
  boxes[0].corners[1]=boxes[0].corners[0];
  assert(!voids_nearest((vec3f_t){10,20,30},&nearest));
  box_count=0;assert(!voids_nearest((vec3f_t){0},&nearest));
  puts("PASS: nearest void boundary, rotated boxes, inside/outside, inactive and unavailable bounds");
  assert(voids_mode==VOIDS_NORMAL && !voids_shown);
  setup(); assert(run()==1 && active_box && sensor_count==1 && !missing_bounds);
  assert(fabsf(drawn_corners[0].x+28.58f)<.001f && drawn_corners[0].y==100 && drawn_corners[7].y==108);
  /* Correctly rotate local bounds, rather than drawing unrotated actor extents. */
  real(A(7)+0x80,0); real(A(7)+0x88,1); real(A(7)+0xA0,-1); real(A(7)+0xA8,0);
  assert(run()==1 && fabsf(drawn_corners[0].x+29)<.001f && fabsf(drawn_corners[0].z-35.58f)<.001f);
  setup(); ptr(A(15)+0x28,A(12)+0x88); assert(run()==0); /* unrelated Area in same module */
  setup(); ptr(A(15)+8,A(14)); assert(run()==0);
  setup(); ptr(A(15),MAIN(Z64HD_VT_ABYSS_CONDITION_NODE));
  assert(run()==1 && active_box); integer(A(19),0); assert(run()==1 && !active_box);
  ptr(A(17),A(18)); assert(run()==0); /* a non-null empty name does not match Link */
  strcpy((char *)A(18),"Obj_BlueFire_A_01"); assert(run()==0);
  strcpy((char *)A(18),"Link"); assert(run()==1);
  blocked=A(17); assert(run()==0); blocked=0;
  integer(A(16),10); assert(run()==0); integer(A(16),8);
  blocked=A(19); assert(run()==0); blocked=0;
  setup(); ptr(A(15),MAIN(Z64HD_VT_SAND_ABYSS_NODE)); assert(run()==1);
  setup(); integer(A(3)+0x38,2); ptr(A(4)+8,A(23)); ptr(A(23)+8,A(5));
  assert(run()==2 && !missing_bounds); /* more than one sensor controller */
  blocked=A(23); assert(run()==1 && missing_bounds==1);
  setup(); ptr(A(9)+0x78,0); assert(run()==0 && missing_bounds==1);
  setup(); real(A(8)+0xC0,NAN); assert(run()==0 && missing_bounds==1);
  setup(); real(A(8)+0x10,-1); assert(run()==0);
  setup(); real(A(7)+0x8C,INFINITY); assert(run()==0);
  setup(); blocked=A(14); assert(run()==0);
  setup(); integer(A(13)+0x60,1025); assert(run()==0);
  setup(); good_list=false; assert(run()==0 && !list_ready);
  setup(); assert(run()==1); voids_shown=false; assert(run()==0);

  /* Only the current Link's abyss messages are blocked, and only in the chosen mode. */
  uint32_t target[3]={42,ABYSS_MESSAGE,0}, payload[3]={8,0,1};
  assert(voids_abyss_message(123,456,target,payload)==17 && calls==1);
  assert(forward_target==target && forward_payload==payload);
  voids_mode=VOIDS_WHILE_LEVITATING; run();
  assert(voids_abyss_message(123,456,target,payload)==17);
  floating=true; run(); assert(voids_abyss_message(123,456,target,payload)==-1);
  target[0]=43; assert(voids_abyss_message(123,456,target,payload)==17); target[0]=42;
  payload[0]=10; assert(voids_abyss_message(123,456,target,payload)==17); payload[0]=8;
  target[1]=123; assert(voids_abyss_message(123,456,target,payload)==17); target[1]=ABYSS_MESSAGE;
  assert(voids_abyss_message(123,456,target,NULL)==17);
  assert(voids_abyss_message(123,456,NULL,payload)==17);
  floating=false; run(); assert(voids_abyss_message(123,456,target,payload)==17);
  voids_mode=VOIDS_DISABLED; run(); assert(voids_abyss_message(123,456,target,payload)==-1);
  integer(player_actor+0x3E8,99); run(); assert(voids_abyss_message(123,456,target,payload)==17);
  target[0]=99; assert(voids_abyss_message(123,456,target,payload)==-1);
  target[1]=SAND_MESSAGE; assert(voids_sand_message(123,456,target,payload)==-1);
  target[0]=42; assert(voids_sand_message(123,456,target,payload)==17);
  target[0]=99; target[1]=ABYSS_MESSAGE; assert(voids_sand_message(123,456,target,payload)==17);
  player_actor=0; run(); assert(voids_abyss_message(123,456,target,payload)==17);

  /* The classifier gate precedes BOTH the Player state write and the global
   * event notification. It must not mutate native objects or block another
   * component, actor, an unknown type, or normal/unlevitated play. */
  setup();
  uintptr_t pass=MAIN(Z64HD_CAVE_START+OOTGZ_VOID_STATE_STUB_OFF+16);
  uintptr_t stop=MAIN(Z64HD_VOID_STATE_RETURN);
  uintptr_t player=A(23);
  ptr(player+0x18,player_actor); ptr(player_actor+0x270,A(20)); ptr(A(20)+0x358,player);
  run(); assert(voids_transition_target(player,1)==pass);
  voids_mode=VOIDS_WHILE_LEVITATING; run(); assert(voids_transition_target(player,1)==pass);
  floating=true; run();
  notice_cooldown=0; notices=logs=0; pending_blocks=0;
  memcpy(before,objects,sizeof(objects));
  for (unsigned type=0; type<32; type++) {
    bool abyss=type==1 || type==3 || type==4 || type==5 || type==7 || type==8 || type==9;
    assert(voids_transition_target(player,type)==(abyss?stop:pass));
  }
  assert(!memcmp(before,objects,sizeof(objects)));
  assert(pending_blocks==0x3BA);
  assert(!notices && !logs); /* hook never calls HUD/logger on a worker */
  run(); assert(notices==1 && logs==1 && !pending_blocks);
  for (int i=0; i<119; i++) { assert(voids_transition_target(player,1)==stop); run(); }
  assert(notices==1); assert(voids_transition_target(player,1)==stop); run(); assert(notices==2);
  floating=false; run(); assert(voids_transition_target(player,1)==pass);
  voids_mode=VOIDS_DISABLED; run(); assert(voids_transition_target(player,1)==stop);
  ptr(A(20)+0x358,A(22)); assert(voids_transition_target(player,1)==pass); ptr(A(20)+0x358,player);
  ptr(player_actor,0); assert(voids_transition_target(player,1)==pass); ptr(player_actor,MAIN(Z64HD_VT_LINKOBJ));
  integer(player_actor+0x3E8,99); assert(voids_transition_target(player,1)==pass);
  run(); assert(voids_transition_target(player,1)==stop);
  blocked=A(23); assert(voids_transition_target(player,1)==pass); blocked=0;
  assert(voids_transition_target(0,1)==pass);
  assert(voids_transition_target(UINTPTR_MAX-7,1)==pass);
  player_actor=0; run(); assert(voids_transition_target(player,1)==pass);
  puts("void state: all native abyss types, owner validation, no object writes, modes and deferred notices passed");
  puts("voids: sensor association, target/reason/condition filters, bounds, read-only inspection and suppression modes passed");
}
