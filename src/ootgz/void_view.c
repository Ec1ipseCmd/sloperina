/* Visibility of mapped void bounds against exact loaded solid collision.
 * Depth and coverage use a fixed screen grid; no graphics/game memory writes. */
#include "void_view.h"
#include "collision_mesh.h"
#include "sys.h"

static cam_t camera_view;
static float visible_range;
static float solid_depth[VOID_VIEW_PIXELS],void_depth[VOID_VIEW_PIXELS]; /* reciprocal view Z */
static uint8_t mask[VOID_VIEW_PIXELS],published[VOID_VIEW_PIXELS];
static bool have_pixels;
static unsigned char publishing;

static float plane_distance(const float p[3],int plane)
{
  switch (plane) {
    case 0:return p[2]-.05f;
    case 1:return p[0]+p[2]*camera_view.tan_x;
    case 2:return p[2]*camera_view.tan_x-p[0];
    case 3:return p[1]+p[2]*camera_view.tan_y;
    default:return p[2]*camera_view.tan_y-p[1];
  }
}

static float edge(const float a[3],const float b[3],float x,float y)
{ return (b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]); }

static void triangle(const float a[3],const float b[3],const float c[3],float *depth)
{
  float area=edge(a,b,c[0],c[1]);
  if (!(area>.00001f || area<-.00001f)) return;
  float lowx=a[0],highx=a[0],lowy=a[1],highy=a[1];
  for (int i=0;i<2;i++) {
    const float *p=i?c:b;
    if (p[0]<lowx) lowx=p[0];
    if (p[0]>highx) highx=p[0];
    if (p[1]<lowy) lowy=p[1];
    if (p[1]>highy) highy=p[1];
  }
  int x0=(int)lowx,x1=(int)highx,y0=(int)lowy,y1=(int)highy;
  if (x0<0) x0=0;
  if (x1>=VOID_VIEW_WIDTH) x1=VOID_VIEW_WIDTH-1;
  if (y0<0) y0=0;
  if (y1>=VOID_VIEW_HEIGHT) y1=VOID_VIEW_HEIGHT-1;
  float inverse=1/area;
  for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++) {
    float w0=edge(b,c,x+.5f,y+.5f)*inverse,w1=edge(c,a,x+.5f,y+.5f)*inverse;
    float w2=1-w0-w1;
    if (w0<-.00001f || w1<-.00001f || w2<-.00001f) continue;
    float z=w0*a[2]+w1*b[2]+w2*c[2];
    unsigned at=(unsigned)(y*VOID_VIEW_WIDTH+x);
    if (z>depth[at]) depth[at]=z;
  }
}

static void polygon(const vec3f_t *vertices,int count,float *depth)
{
  float p[16][3],next[16][3];
  for (int i=0;i<count;i++) {
    float world[3]={vertices[i].x,vertices[i].y,vertices[i].z};
    to_view(&camera_view,world,p[i]);
  }
  for (int plane=0;plane<5 && count;plane++) {
    int n=0;
    for (int i=0;i<count;i++) {
      float *a=p[i],*b=p[(i+1)%count],da=plane_distance(a,plane),db=plane_distance(b,plane);
      if ((da>=0)!=(db>=0)) {
        if (n==16) return;
        float t=da/(da-db);
        for (int j=0;j<3;j++) next[n][j]=a[j]+t*(b[j]-a[j]);
        n++;
      }
      if (db>=0) { if (n==16) return;memcpy(next[n++],b,3*sizeof(float)); }
    }
    count=n;memcpy(p,next,(size_t)count*sizeof(p[0]));
  }
  for (int i=0;i<count;i++) {
    float x,y;to_screen(&camera_view,p[i],&x,&y);
    if (!(x>-.001f && x<1.001f && y>-.001f && y<1.001f && p[i][2]>.049f)) return;
    p[i][0]=x*VOID_VIEW_WIDTH;p[i][1]=y*VOID_VIEW_HEIGHT;p[i][2]=1/p[i][2];
  }
  for (int i=1;i+1<count;i++) triangle(p[0],p[i],p[i+1],depth);
}

void void_view_begin(const cam_t *cam,float range)
{
  camera_view=*cam;visible_range=range>0?range:0;
  memset(solid_depth,0,sizeof(solid_depth));memset(void_depth,0,sizeof(void_depth));
  memset(mask,0,sizeof(mask));
}

void void_view_occluder(const vec3f_t v[3],int kind,void *ctx)
{ (void)ctx;if (kind!=COLLISION_TRIGGER && visible_range>0) polygon(v,3,solid_depth); }

void void_view_box(const vec3f_t corners[8],bool active,void *ctx)
{
  (void)ctx;
  if (!active || visible_range<=0) return;
  for (int axis=0;axis<3;axis++) for (int side=0;side<2;side++) {
    int a=side<<axis,u=1<<((axis+1)%3),v=1<<((axis+2)%3);
    vec3f_t face[4]={corners[a],corners[a|u],corners[a|u|v],corners[a|v]};
    polygon(face,4,void_depth);
  }
}

void void_view_finish(void)
{
  bool any=false;
  for (int y=0;y<VOID_VIEW_HEIGHT;y++) for (int x=0;x<VOID_VIEW_WIDTH;x++) {
    unsigned at=(unsigned)(y*VOID_VIEW_WIDTH+x);
    if (!(void_depth[at]>0)) continue;
    float z=1/void_depth[at];
    float vx=((x+.5f)*2/VOID_VIEW_WIDTH-1)*camera_view.tan_x;
    float vy=(1-(y+.5f)*2/VOID_VIEW_HEIGHT)*camera_view.tan_y;
    if (z*z*(1+vx*vx+vy*vy)>=visible_range*visible_range) continue;
    /* Conservative one-cell blocker expansion prevents a bright edge leaking
     * onto a nearby wall when the mask is enlarged to framebuffer resolution. */
    float blocker=0;
    for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++) {
      int xx=x+dx,yy=y+dy;
      if (xx>=0 && yy>=0 && xx<VOID_VIEW_WIDTH && yy<VOID_VIEW_HEIGHT &&
          solid_depth[yy*VOID_VIEW_WIDTH+xx]>blocker) blocker=solid_depth[yy*VOID_VIEW_WIDTH+xx];
    }
    if (blocker>0 && z+.02f>=1/blocker) continue;
    mask[at]=1;any=true;
  }
  if (!__atomic_test_and_set(&publishing,__ATOMIC_ACQUIRE)) {
    memcpy(published,mask,sizeof(mask));have_pixels=any;
    __atomic_clear(&publishing,__ATOMIC_RELEASE);
  }
}

void void_view_clear(void)
{
  memset(mask,0,sizeof(mask));
  if (!__atomic_test_and_set(&publishing,__ATOMIC_ACQUIRE)) {
    have_pixels=false;
    __atomic_clear(&publishing,__ATOMIC_RELEASE);
  }
}

bool void_view_copy(uint8_t out[VOID_VIEW_PIXELS])
{
  if (__atomic_test_and_set(&publishing,__ATOMIC_ACQUIRE)) return false;
  bool any=have_pixels;if (any) memcpy(out,published,sizeof(published));
  __atomic_clear(&publishing,__ATOMIC_RELEASE);
  return any;
}

bool void_view_nearest(vec3f_t player,void_nearest_t *out)
{
  float sqrtf(float);float best=1e30f;bool found=false;
  for (int y=0;y<VOID_VIEW_HEIGHT;y++) for (int x=0;x<VOID_VIEW_WIDTH;x++) {
    unsigned at=(unsigned)(y*VOID_VIEW_WIDTH+x);if (!mask[at]) continue;
    float z=1/void_depth[at],vx=((x+.5f)*2/VOID_VIEW_WIDTH-1)*camera_view.tan_x*z;
    float vy=(1-(y+.5f)*2/VOID_VIEW_HEIGHT)*camera_view.tan_y*z;
    vec3f_t q={camera_view.pos[0]+camera_view.forward[0]*z+camera_view.right[0]*vx+camera_view.up[0]*vy,
               camera_view.pos[1]+camera_view.forward[1]*z+camera_view.right[1]*vx+camera_view.up[1]*vy,
               camera_view.pos[2]+camera_view.forward[2]*z+camera_view.right[2]*vx+camera_view.up[2]*vy};
    float dx=q.x-player.x,dy=q.y-player.y,dz=q.z-player.z,d=dx*dx+dy*dy+dz*dz;
    if (d<best) { best=d;out->point=q;found=true; }
  }
  if (found) {
    float x=out->point.x-player.x,y=out->point.y-player.y,z=out->point.z-player.z;
    out->distance=sqrtf(x*x+y*y+z*z);out->inside=false;
  }
  return found;
}

uint32_t void_view_blend(uint32_t background)
{
  /* RGB 255,64,40. Each channel is rounded arithmetic mean; alpha unchanged. */
  uint32_t r=((background&255)+255+1)/2;
  uint32_t g=(((background>>8)&255)+64+1)/2;
  uint32_t b=(((background>>16)&255)+40+1)/2;
  return (background&0xFF000000u)|r|(g<<8)|(b<<16);
}
