/* Synthetic collision data only; no game files are distributed with tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ootgz/collision_mesh.c"
static unsigned totals[COLLISION_KINDS];
static vec3f_t last[3];
static void collect(const vec3f_t v[3],int kind,void *ctx)
{ (void)ctx; assert(kind>=0&&kind<COLLISION_KINDS); totals[kind]++; memcpy(last,v,sizeof(last)); }
static void w(void *b,size_t at,uint32_t v) { memcpy((char *)b+at,&v,4); }
static void q(void *b,size_t at,uint64_t v) { memcpy((char *)b+at,&v,8); }
static void f32(void *b,size_t at,float v) { memcpy((char *)b+at,&v,4); }
static void rv(void *b,size_t at,size_t target,unsigned n) { w(b,at,(uint32_t)(target-at));w(b,at+4,n); }
static void header(void *b,size_t at,const char *name,unsigned size)
{ w(b,at,__builtin_bswap32(size));memcpy((char *)b+at+4,name,4); }
static void mesh_test(void)
{
  unsigned char b[4096]={0},masks[16]={0},materials[16]={0};
  q(b,0,0x100000000ull+Z64HD_VT_HK_MESH);
  rv(b,0x88,0x100,1); rv(b,0x60,0x200,1); w(b,0x204,0);
  rv(b,0x108,0x300,2); rv(b,0x110,0x400,40); /* trailing padding is not another vertex */
  for (int i=0;i<3;i++) w(b,0x120+i*4,INT32_MAX);
  vec3f_t vertices[3]={{0,0,0},{0,0,1},{1,0,0}};
  memcpy(b+0x400,vertices,sizeof(vertices));
  memcpy(b+0x300,(unsigned char[]){0,1,2,2,0,2,1,1},8);
  q(masks,0,8); filter_t filter={{materials,16},{masks,16},0,1,-1};
  /* Standalone actor shape: same mesh, placed through its native body matrix. */
  unsigned char file[4352]={0};
  memcpy(file,"Phive\0",6);file[6]=1;file[11]=3;
  w(file,12,48);w(file,16,4240);w(file,20,4256);
  w(file,24,4192);w(file,28,16);w(file,32,16);
  header(file,48,"TAG0",4192);header(file,56,"DATA",4104);
  memcpy(file+64,b,sizeof(b));
  header(file,4160,"INDX",80);header(file,4168,"ITEM",72);
  w(file,4188,0x10000001);w(file,4192,0);w(file,4196,1);
  q(file,4256,8);
  transform_t placed=identity;placed.m[0][3]=10;placed.m[1][3]=20;
  collision_scan_t actor_scan={.remaining=100,.main=0x100000000ull,.kinds=15,.triangle=collect};
  memset(totals,0,sizeof(totals));
  collision_shape_scan(file,sizeof(file),placed.m,2,&actor_scan);
  assert(totals[COLLISION_FLOOR]==1 && totals[COLLISION_CEILING]==1);
  assert(last[0].x==10 && last[0].y==20 && last[1].x==12);
  for (size_t n=0;n<48;n++) collision_shape_scan(file,n,placed.m,2,&actor_scan);
  collision_shape_scan(file,sizeof(file),placed.m,0,&actor_scan);
  assert(totals[COLLISION_FLOOR]==1);
  collision_scan_t s={.remaining=1,.main=0x100000000ull,.kinds=15,.triangle=collect};
  memset(totals,0,sizeof(totals));
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_FLOOR]==1&&s.visited==1&&!s.remaining);
  s.skip=1;s.remaining=1;s.visited=0;
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_CEILING]==1&&s.visited==1);
  s=(collision_scan_t){.remaining=100,.main=0x100000000ull,.kinds=2,.triangle=collect};
  vertices[1]=(vec3f_t){0,1,0};memcpy(b+0x400,vertices,sizeof(vertices));
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_WALL]==2);
  q(masks,0,0);s.remaining=100;
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_WALL]==2);
  q(masks,0,8);w(materials,0,27);s.remaining=100;
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_WALL]==2); /* invisible barriers excluded */
  filter.kind=COLLISION_TRIGGER;s.kinds=8;s.remaining=100;
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_TRIGGER]==2);
  /* Invalid relative arrays, indices, object types, finite coordinates and truncation. */
  b[0x300]=200;s.remaining=100;
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_TRIGGER]==3);
  w(b,0x108,0x80000000);s.remaining=100;
  shape((bytes_t){b,sizeof(b)},0,&identity,filter,(bytes_t){0},&s,0);
  assert(totals[COLLISION_TRIGGER]==3);
  for (size_t size=0;size<sizeof(b);size++) {
    s.remaining=8;s.nodes=0;
    shape((bytes_t){b,size},0,&identity,filter,(bytes_t){0},&s,0);
  }
  transform_t out;
  memset(b,0,64);f32(b,12,1);f32(b,16,7);f32(b,32,2);f32(b,36,3);f32(b,40,4);
  assert(transform((bytes_t){b,64},0,&identity,&out));
  vec3f_t v=place(&out,(vec3f_t){1,1,1});assert(v.x==9&&v.y==3&&v.z==4);
  f32(b,12,0);assert(!transform((bytes_t){b,64},0,&identity,&out));
}

static void containers(void)
{
  unsigned char b[256]={0};
  header(b,0,"TAG0",128);header(b,8,"DATA",64);
  header(b,72,"INDX",56);header(b,80,"ITEM",48);
  w(b,100,0x10000001);w(b,104,0);w(b,108,1);
  bytes_t data;size_t root;
  assert(tag_root((bytes_t){b,128},&data,&root)&&data.size==56&&root==0);
  w(b,104,10000);assert(!tag_root((bytes_t){b,128},&data,&root));
  for (unsigned i=0;i<128;i++) assert(!tag_root((bytes_t){b,i},&data,&root));
  memcpy(b,"Phive\0",6);b[6]=2;b[7]=2;b[10]=2;b[11]=12;
  /* Invalid sections must not escape the containing file. */
  w(b,12+11*4,UINT32_MAX);w(b,12+(12+11)*4,32);
  collision_scan_t s={.remaining=64,.kinds=15,.triangle=collect};
  collision_mesh_scan(b,sizeof(b),&s);assert(!s.visited);
}

int main(int argc,char **argv)
{
  mesh_test();containers();
  puts("PASS: bounded relative arrays, primitive continuation, filters, masks, transforms and malformed collision chunks");
  if (argc==3) { /* Optional private, read-only validation of a locally captured loaded tile. */
    FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);
    void *data=malloc((size_t)size);assert(data&&fread(data,1,(size_t)size,file)==(size_t)size);fclose(file);
    memset(totals,0,sizeof(totals));
    collision_scan_t s={.remaining=10000000,.kinds=15,.main=strtoull(argv[2],NULL,0),.triangle=collect};
    bool standalone=size>12 && ((unsigned char *)data)[10]==0;
    if (standalone) collision_shape_scan(data,(size_t)size,identity.m,1,&s);
    else collision_mesh_scan(data,(size_t)size,&s);
    printf("tile: floor %u wall %u ceiling %u trigger %u; %zu primitives, %u unsupported shapes\n",
           totals[0],totals[1],totals[2],totals[3],s.visited,s.unsupported);
    unsigned full[4];memcpy(full,totals,sizeof(full));memset(totals,0,sizeof(totals));
    size_t cursor=0;unsigned frames=0;
    do { s=(collision_scan_t){.remaining=4096,.skip=cursor,.kinds=15,.main=strtoull(argv[2],NULL,0),.triangle=collect};
      if (standalone) collision_shape_scan(data,(size_t)size,identity.m,1,&s);
      else collision_mesh_scan(data,(size_t)size,&s);
      cursor+=s.visited;assert(++frames<4096);
    } while (!s.remaining);
    assert(!memcmp(full,totals,sizeof(full)));printf("PASS: incremental scan matches full scan in %u batches\n",frames);
    free(data);
  }
  return 0;
}
