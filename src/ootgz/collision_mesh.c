/* The relative arrays match tools/phive.py. Never retain pointers into a tile.
 * Work is capped by primitive and node budgets, including malformed input. */
#include "collision_mesh.h"
#include "sys.h"
#include "z64hd.h"

typedef struct { const uint8_t *data; size_t size; } bytes_t;
typedef struct { float m[3][4]; } transform_t;
typedef struct { bytes_t materials, masks; unsigned first, count; int kind; } filter_t;

static const transform_t identity = { { {1,0,0,0}, {0,1,0,0}, {0,0,1,0} } };
static bool range(bytes_t b, size_t at, size_t n) { return at <= b.size && n <= b.size - at; }
static uint32_t word(bytes_t b, size_t at) { uint32_t v=0; if (range(b,at,4)) memcpy(&v,b.data+at,4); return v; }
static uint64_t wide(bytes_t b, size_t at) { uint64_t v=0; if (range(b,at,8)) memcpy(&v,b.data+at,8); return v; }
static float real(bytes_t b, size_t at) { uint32_t v=word(b,at); float f; memcpy(&f,&v,4); return f; }
static bool coord_finite(float v) { return v > -1000000 && v < 1000000; }
static bool equal(const void *a, const void *b, size_t n)
{ const uint8_t *x=a,*y=b; for (size_t i=0;i<n;i++) if (x[i]!=y[i]) return false; return true; }
static bytes_t slice(bytes_t b, size_t at, size_t n)
{ return range(b,at,n) ? (bytes_t){b.data+at,n} : (bytes_t){0}; }

static bool relative(bytes_t b, size_t at, int64_t offset, size_t n, size_t *out)
{
  if (at > b.size || offset < -(int64_t)at || offset > (int64_t)(b.size-at)) return false;
  *out = (size_t)((int64_t)at+offset);
  return range(b,*out,n);
}

static bool array(bytes_t b, size_t at, bool large, size_t stride, size_t *data, unsigned *count)
{
  if (!range(b,at,large?12:8)) return false;
  *count=word(b,at+(large?8:4));
  if (*count>1000000) return false;
  int64_t offset=large?(int64_t)wide(b,at):(int32_t)word(b,at);
  return relative(b,at,offset,(size_t)*count*stride,data);
}

static bytes_t section(bytes_t file, unsigned i)
{
  if (file.size<12 || !equal(file.data,"Phive\0",6) || i>=file.data[11]) return (bytes_t){0};
  unsigned n=file.data[11];
  if (!range(file,12,n*8)) return (bytes_t){0};
  return slice(file,word(file,12+i*4),word(file,12+(n+i)*4));
}

static bytes_t chunk(bytes_t b, const char *tag)
{
  for (size_t at=0; range(b,at,8);) {
    uint32_t size=__builtin_bswap32(word(b,at))&0x3FFFFFFF;
    if (size<8 || !range(b,at,size)) break;
    if (equal(b.data+at+4,tag,4)) return slice(b,at+8,size-8);
    at+=size;
  }
  return (bytes_t){0};
}

static bool tag_root(bytes_t file, bytes_t *data, size_t *root)
{
  bytes_t tag=chunk(file,"TAG0");
  *data=chunk(tag,"DATA");
  bytes_t items=chunk(chunk(tag,"INDX"),"ITEM");
  if (!range(items,12,12) || !(word(items,12)&0x10000000)) return false;
  *root=word(items,16);
  return range(*data,*root,0x38);
}

static vec3f_t place(const transform_t *t, vec3f_t p)
{
  return (vec3f_t){t->m[0][0]*p.x+t->m[0][1]*p.y+t->m[0][2]*p.z+t->m[0][3],
                   t->m[1][0]*p.x+t->m[1][1]*p.y+t->m[1][2]*p.z+t->m[1][3],
                   t->m[2][0]*p.x+t->m[2][1]*p.y+t->m[2][2]*p.z+t->m[2][3]};
}

static bool transform(bytes_t b, size_t at, const transform_t *parent, transform_t *out)
{
  if (!range(b,at,48)) return false;
  float x=real(b,at),y=real(b,at+4),z=real(b,at+8),w=real(b,at+12);
  float norm=x*x+y*y+z*z+w*w;
  if (!(norm>.99f && norm<1.01f)) return false;
  float r[3][3]={{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)},
                 {2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)},
                 {2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}};
  for (int i=0;i<3;i++) for (int j=0;j<4;j++) {
    float v=j==3?parent->m[i][3]:0;
    for (int k=0;k<3;k++) v+=parent->m[i][k]*(j==3?real(b,at+16+k*4):r[k][j]*real(b,at+32+j*4));
    if (!coord_finite(v)) return false;
    out->m[i][j]=v;
  }
  return true;
}

static bool consume(collision_scan_t *s, size_t n)
{
  if (!s->remaining) return false;
  if (s->skip>=n) { s->skip-=n; return false; }
  return true;
}

static bool material(filter_t f, unsigned tag)
{
  if (f.kind==COLLISION_TRIGGER) return true;
  uint64_t index=(uint64_t)f.first+(tag<f.count?tag:0);
  if (!range(f.masks,(size_t)index*16,16) || !range(f.materials,(size_t)index*16,16)) return false;
  /* Player layer bit. Invisible AirWall materials use the existing viewer. */
  return (wide(f.masks,(size_t)index*16)&8) && word(f.materials,(size_t)index*16)!=27;
}

static void emit(const vec3f_t local[3], const transform_t *t, filter_t f, unsigned tag, collision_scan_t *s)
{
  if (!material(f,tag)) return;
  vec3f_t v[3];
  for (int i=0;i<3;i++) {
    v[i]=place(t,local[i]);
    if (!coord_finite(v[i].x)||!coord_finite(v[i].y)||!coord_finite(v[i].z)) return;
  }
  float ax=v[1].x-v[0].x,ay=v[1].y-v[0].y,az=v[1].z-v[0].z;
  float bx=v[2].x-v[0].x,by=v[2].y-v[0].y,bz=v[2].z-v[0].z;
  float nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx;
  float length=nx*nx+ny*ny+nz*nz;
  if (!(length>1e-12f && length<1e20f)) return;
  int kind=f.kind;
  if (kind!=COLLISION_TRIGGER) kind=ny*ny>.25f*length ? (ny>0?COLLISION_FLOOR:COLLISION_CEILING) : COLLISION_WALL;
  if (s->kinds&(1u<<kind)) s->triangle(v,kind,s->ctx);
}

static unsigned mesh_tag(bytes_t b, size_t table, unsigned count, uint32_t key)
{
  /* Runs are ordered; binary search the last start <= key. */
  unsigned lo=0,hi=count;
  while (lo<hi) { unsigned mid=lo+(hi-lo)/2; if (word(b,table+mid*8)<=key) lo=mid+1; else hi=mid; }
  return lo ? word(b,table+(lo-1)*8+4)&0xFFFF : 0;
}

static void mesh(bytes_t b, size_t o, const transform_t *t, filter_t f, collision_scan_t *s)
{
  size_t sections,tags; unsigned count,ntags;
  if (!array(b,o+0x88,false,64,&sections,&count) || count>65536 ||
      !array(b,o+0x60,false,8,&tags,&ntags)) return;
  float scale[3]={real(b,o+0x50),real(b,o+0x54),real(b,o+0x58)};
  for (unsigned j=0;j<count && s->remaining;j++) {
    size_t sec=sections+j*64,prims,verts; unsigned np,nv;
    if (!array(b,sec+8,false,4,&prims,&np)||np>256||!array(b,sec+16,false,1,&verts,&nv)) continue;
    int32_t offset[3]={(int32_t)word(b,sec+32),(int32_t)word(b,sec+36),(int32_t)word(b,sec+40)};
    bool plain=offset[0]==INT32_MAX&&offset[1]==INT32_MAX&&offset[2]==INT32_MAX;
    unsigned stride=plain?12:6;
    if (nv/stride>256) continue; /* vertex byte buffers may include trailing padding */
    if (!consume(s,np)) continue;
    vec3f_t vertices[256];
    for (unsigned k=0;k<nv/stride;k++) {
      float v[3];
      for (int axis=0;axis<3;axis++) {
        if (plain) v[axis]=real(b,verts+k*stride+axis*4);
        else {
          uint16_t q; memcpy(&q,b.data+verts+k*stride+axis*2,2);
          v[axis]=((float)offset[axis]+q)*scale[axis];
        }
      }
      vertices[k]=(vec3f_t){v[0],v[1],v[2]};
    }
    unsigned start=(unsigned)s->skip; s->skip=0;
    for (unsigned k=start;k<np && s->remaining;k++) {
      s->remaining--; s->visited++;
      const uint8_t *idx=b.data+prims+k*4;
      if (idx[0]>=nv/stride||idx[1]>=nv/stride||idx[2]>=nv/stride||idx[3]>=nv/stride) continue;
      unsigned tag=mesh_tag(b,tags,ntags,(j<<9)|(k<<1));
      vec3f_t v[3]={vertices[idx[0]],vertices[idx[1]],vertices[idx[2]]};
      emit(v,t,f,tag,s);
      if (idx[3]!=idx[2]) { v[1]=v[2]; v[2]=vertices[idx[3]]; emit(v,t,f,tag,s); }
    }
  }
}

static void shape(bytes_t, size_t, const transform_t *, filter_t, bytes_t, collision_scan_t *, unsigned);

static void compound(bytes_t b, size_t o, const transform_t *t, filter_t f, bytes_t info, collision_scan_t *s, unsigned depth)
{
  size_t inst; unsigned count;
  if (!array(b,o+0x38,true,64,&inst,&count)||count>8192) return;
  for (unsigned i=0;i<count && s->remaining && s->nodes<16384;i++) {
    size_t at=inst+i*64,child;
    int64_t rel=(int64_t)wide(b,at+48);
    if (!rel||!relative(b,at+48,rel,0x38,&child)) continue;
    filter_t filter=f;
    if (info.size) {
      unsigned index=word(b,at+44)&0xFFFF;
      if (!range(info,(size_t)index*20,20)) continue;
      const uint8_t *item=info.data+index*20;
      if (item[4]) {
        if (!(s->kinds&(1u<<COLLISION_TRIGGER)) || (item[5]!=13 && item[5]!=14)) continue;
        filter.kind=COLLISION_TRIGGER;
      } else if (item[5]==8) continue; /* duplicate Rough mesh */
      filter.first=word(info,index*20+12); filter.count=word(info,index*20+16);
    }
    transform_t world;
    if (transform(b,at,t,&world)) shape(b,child,&world,filter,(bytes_t){0},s,depth+1);
  }
}

static void shape(bytes_t b, size_t o, const transform_t *t, filter_t f, bytes_t info, collision_scan_t *s, unsigned depth)
{
  if (depth>8 || ++s->nodes>16384 || !s->remaining || !range(b,o,0x58)) return;
  uint64_t vt=wide(b,o);
  if (vt==s->main+Z64HD_VT_HK_COMPOUND) { compound(b,o,t,f,info,s,depth); return; }
  if (vt==s->main+Z64HD_VT_HK_MESH) { mesh(b,o,t,f,s); return; }
  if (vt!=s->main+Z64HD_VT_HK_BOX && vt!=s->main+Z64HD_VT_HK_CONVEX && vt!=s->main+Z64HD_VT_HK_CYLINDER) {
    s->unsupported++; return;
  }
  size_t verts,faces,indices; unsigned nv,nf,ni;
  if (!array(b,o+0x38,false,12,&verts,&nv)||nv>256 || !array(b,o+0x48,false,4,&faces,&nf)||nf>256 ||
      !array(b,o+0x50,false,1,&indices,&ni)) return;
  for (unsigned face=0;face<nf && s->remaining;face++) {
    unsigned desc=word(b,faces+face*4),first=desc&65535,n=(desc>>16)&255;
    if (n<3||first>ni||n>ni-first) continue;
    if (!consume(s,n-2)) continue;
    unsigned start=(unsigned)s->skip+1; s->skip=0;
    for (unsigned i=start;i+1<n && s->remaining;i++) {
      s->remaining--; s->visited++;
      unsigned ids[3]={b.data[indices+first],b.data[indices+first+i],b.data[indices+first+i+1]};
      if (ids[0]>=nv||ids[1]>=nv||ids[2]>=nv) continue;
      vec3f_t v[3];
      for (int j=0;j<3;j++) v[j]=(vec3f_t){real(b,verts+ids[j]*12),real(b,verts+ids[j]*12+4),real(b,verts+ids[j]*12+8)};
      emit(v,t,f,0,s);
    }
  }
}

void collision_mesh_scan(const void *bytes, size_t size, collision_scan_t *s)
{
  bytes_t file={bytes,size};
  if (size<12 || !equal(bytes,"Phive\0",6) || file.data[10]!=2 || file.data[6]!=2 || file.data[7]!=2) return;
  bytes_t info=section(file,4),materials=section(file,5),masks=section(file,6);
  filter_t f={materials,masks,0,0,-1};
  transform_t ground=identity;
  /* Placement and shape sections need not be adjacent or ordered. */
  for (unsigned i=11;i<file.data[11];i++) {
    bytes_t sec=section(file,i);
    if (sec.size==48)
      for (int j=0;j<3;j++) { ground.m[j][j]=real(sec,j*4); ground.m[j][3]=real(sec,24+j*4); }
  }
  for (unsigned i=11;i<file.data[11] && s->remaining;i++) {
    bytes_t sec=section(file,i),data; size_t root;
    if (tag_root(sec,&data,&root)) shape(data,root,&identity,f,info,s,0);
    else if (sec.size>=12 && equal(sec.data,"Phive\0",6) && sec.data[10]==0) {
      bytes_t tag=section(sec,0);
      filter_t g={section(sec,1),section(sec,2),0,0,-1};
      g.count=(unsigned)(g.materials.size/16);
      if (tag_root(tag,&data,&root)) shape(data,root,&ground,g,(bytes_t){0},s,0);
    }
  }
}
