/* Visibility of mapped void bounds against exact loaded solid collision.
 * Depth and coverage use a fixed screen grid; no graphics/game memory writes. */
#include "void_view.h"
#include "collision_mesh.h"
#include "sys.h"

static cam_t camera_view;
static float visible_range;
static float solid_depth[VOID_VIEW_PIXELS],void_depth[VOID_VIEW_PIXELS]; /* reciprocal view Z */
static uint8_t mask[VOID_VIEW_PIXELS];
static float solid_dx[VOID_VIEW_PIXELS],solid_dy[VOID_VIEW_PIXELS];
static view_image_t pending,published;
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
    if (z>depth[at]) {
      depth[at]=z;
      if (depth==solid_depth) {
        solid_dx[at]=((b[1]-c[1])*(a[2]-c[2])+(c[1]-a[1])*(b[2]-c[2]))*inverse;
        solid_dy[at]=((c[0]-b[0])*(a[2]-c[2])+(a[0]-c[0])*(b[2]-c[2]))*inverse;
      }
    }
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
  pending.count=0;pending.hide_link=false;
  memset(solid_depth,0,sizeof(solid_depth));memset(void_depth,0,sizeof(void_depth));
  memset(mask,0,sizeof(mask));
}

void void_view_occluder(const vec3f_t v[3],int kind,void *ctx)
{ (void)ctx;if (kind!=COLLISION_TRIGGER && visible_range>0) polygon(v,3,solid_depth); }

void void_view_box(const vec3f_t corners[8],bool active,void *ctx)
{
  (void)ctx;
  if (!active || visible_range<=0) return;
  for (int i=0;i<8;i++) for (int axis=0;axis<3;axis++)
    if (!(i&(1<<axis))) void_view_line(corners[i],corners[i|(1<<axis)],VIEW_VOID);
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
    memcpy(pending.mask,mask,sizeof(mask));published=pending;
    have_pixels=any || pending.count;
    __atomic_clear(&publishing,__ATOMIC_RELEASE);
  }
}

void void_view_clear(void)
{
  memset(mask,0,sizeof(mask));pending.count=0;
  if (!__atomic_test_and_set(&publishing,__ATOMIC_ACQUIRE)) {
    have_pixels=false;
    __atomic_clear(&publishing,__ATOMIC_RELEASE);
  }
}

bool void_view_copy(uint8_t out[VOID_VIEW_PIXELS])
{
  if (__atomic_test_and_set(&publishing,__ATOMIC_ACQUIRE)) return false;
  bool any=have_pixels;if (any) memcpy(out,published.mask,sizeof(published.mask));
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

/* Clip world edges before projection, then split only where visibility changes.
 * Keep screen coordinates as floats: line width is chosen in actual output
 * pixels, never by enlarging a coarse coverage raster. */
static void line_run(const float a[3],const float b[3],float start,float end,uint8_t color)
{
  if (pending.count==VIEW_LINES) return;
  pending.lines[pending.count++]=(view_line_t){a[0]+(b[0]-a[0])*start,a[1]+(b[1]-a[1])*start,
                                             a[0]+(b[0]-a[0])*end,a[1]+(b[1]-a[1])*end,color};
}

void void_view_line(vec3f_t from,vec3f_t to,uint8_t color)
{
  if (visible_range<=0 || !color || color>=VIEW_COLORS) return;
  float wa[3]={from.x,from.y,from.z},wb[3]={to.x,to.y,to.z},a[3],b[3];
  to_view(&camera_view,wa,a);to_view(&camera_view,wb,b);
  for (int plane=0;plane<5;plane++) {
    float da=plane_distance(a,plane),db=plane_distance(b,plane);
    if (!(da>=0) && !(db>=0)) return;
    if ((da>=0)!=(db>=0)) {
      float t=da/(da-db),cut[3];
      for (int j=0;j<3;j++) cut[j]=a[j]+t*(b[j]-a[j]);
      memcpy(da<0?a:b,cut,sizeof(cut));
    }
  }
  for (int i=0;i<2;i++) {
    float *p=i?b:a,x,y;to_screen(&camera_view,p,&x,&y);
    if (!(x>-.001f && x<1.001f && y>-.001f && y<1.001f && p[2]>.049f)) return;
    p[0]=x;p[1]=y;p[2]=1/p[2];
  }
  float dx=__builtin_fabsf(b[0]-a[0])*VOID_VIEW_WIDTH,dy=__builtin_fabsf(b[1]-a[1])*VOID_VIEW_HEIGHT;
  int steps=(int)((dx>dy?dx:dy)*2)+1,begin=-1;
  for (int i=0;i<=steps;i++) {
    float t=(float)i/steps,x=a[0]+(b[0]-a[0])*t,y=a[1]+(b[1]-a[1])*t;
    float inverse=a[2]+(b[2]-a[2])*t,z=1/inverse;
    float rx=(x*2-1)*camera_view.tan_x,ry=(1-y*2)*camera_view.tan_y;
    int ix=(int)(x*VOID_VIEW_WIDTH),iy=(int)(y*VOID_VIEW_HEIGHT);
    if (ix<0) ix=0;
    if (iy<0) iy=0;
    if (ix>=VOID_VIEW_WIDTH) ix=VOID_VIEW_WIDTH-1;
    if (iy>=VOID_VIEW_HEIGHT) iy=VOID_VIEW_HEIGHT-1;
    unsigned at=(unsigned)(iy*VOID_VIEW_WIDTH+ix);
    float blocker=solid_depth[at];
    /* Evaluate the selected triangle's depth at the edge's subpixel location.
     * This avoids a staircase of self-occlusion on sloped floors and walls. */
    if (blocker>0) blocker+=solid_dx[at]*(x*VOID_VIEW_WIDTH-ix-.5f)+solid_dy[at]*(y*VOID_VIEW_HEIGHT-iy-.5f);
    bool visible=z*z*(1+rx*rx+ry*ry)<visible_range*visible_range &&
                 (!(blocker>0) || z<=1/blocker+.015f);
    if (visible && begin<0) begin=i;
    if ((!visible || i==steps) && begin>=0) {
      line_run(a,b,(float)begin/steps,(float)(visible?i:i-1)/steps,color);begin=-1;
    }
  }
}

bool void_view_link(vec3f_t feet,vec3f_t head)
{
  /* The renderer has no verified skinned-player depth target. Reserve the
   * projected body/attachment envelope so geometry never paints over Link.
   * This deliberately leaves some empty space around him, not a fake silhouette. */
  float low[3]={feet.x-.7f,feet.y-.06f,feet.z-.7f};
  float high[3]={feet.x+.7f,head.y+.05f,feet.z+.7f};
  if (!(head.y>=feet.y && head.y-feet.y<5)) return false;
  if (head.x-.7f<low[0]) low[0]=head.x-.7f;
  if (head.x+.7f>high[0]) high[0]=head.x+.7f;
  if (head.z-.7f<low[2]) low[2]=head.z-.7f;
  if (head.z+.7f>high[2]) high[2]=head.z+.7f;
  pending.link[0]=pending.link[1]=1;pending.link[2]=pending.link[3]=0;
  float vertices[8][3],nearest=1e30f,farthest=-1e30f;
  for (int i=0;i<8;i++) {
    float p[3];
    for (int axis=0;axis<3;axis++) p[axis]=i&(1<<axis)?high[axis]:low[axis];
    to_view(&camera_view,p,vertices[i]);
    if (vertices[i][2]<nearest) nearest=vertices[i][2];
    if (vertices[i][2]>farthest) farthest=vertices[i][2];
  }
  if (farthest<=.05f) return true; /* Link entirely behind the camera */
  if (nearest<=.05f) {
    pending.link[0]=pending.link[1]=0;pending.link[2]=pending.link[3]=1;
    pending.hide_link=true;return true;
  }
  for (int i=0;i<8;i++) {
    float *v=vertices[i],x,y;
    to_screen(&camera_view,v,&x,&y);
    if (x<pending.link[0]) pending.link[0]=x;
    if (y<pending.link[1]) pending.link[1]=y;
    if (x>pending.link[2]) pending.link[2]=x;
    if (y>pending.link[3]) pending.link[3]=y;
  }
  if (pending.link[2]<0 || pending.link[0]>1 || pending.link[3]<0 || pending.link[1]>1) return true;
  for (int i=0;i<4;i++) {
    if (pending.link[i]<0) pending.link[i]=0;
    if (pending.link[i]>1) pending.link[i]=1;
  }
  pending.hide_link=true;return true;
}

bool void_view_snapshot(view_image_t *out)
{
  if (__atomic_test_and_set(&publishing,__ATOMIC_ACQUIRE)) return false;
  bool any=have_pixels;if (any) *out=published;
  __atomic_clear(&publishing,__ATOMIC_RELEASE);return any;
}

void void_view_band(const view_image_t *view,int width,int height,int y,int rows,uint8_t *out)
{
  memset(out,0,(size_t)width*rows);
  for (int yy=0;yy<rows;yy++) {
    const uint8_t *row=view->mask+(y+yy)*VOID_VIEW_HEIGHT/height*VOID_VIEW_WIDTH;
    for (int x=0;x<VOID_VIEW_WIDTH;) {
      if (!row[x]) { x++;continue; }
      int start=x++;
      while (x<VOID_VIEW_WIDTH && row[x]) x++;
      int x0=(start*width+VOID_VIEW_WIDTH-1)/VOID_VIEW_WIDTH;
      int x1=(x*width+VOID_VIEW_WIDTH-1)/VOID_VIEW_WIDTH;
      memset(out+(size_t)yy*width+x0,VIEW_VOID,(size_t)(x1-x0));
    }
  }
  for (unsigned i=0;i<view->count;i++) {
    view_line_t line=view->lines[i];
    float x0=(float)(int)(line.x0*(width-1)+.5f),y0=(float)(int)(line.y0*(height-1)+.5f);
    float dx=(float)(int)(line.x1*(width-1)+.5f)-x0,dy=(float)(int)(line.y1*(height-1)+.5f)-y0;
    if ((y0<y-1 && y0+dy<y-1) || (y0>y+rows && y0+dy>y+rows)) continue;
    float ax=__builtin_fabsf(dx),ay=__builtin_fabsf(dy);
    int steps=(int)(ax>ay?ax:ay);if (!steps) steps=1;
    int first=0,last=steps;
    if (dy>1 || dy<-1) {
      float a=(y-1-y0)/dy,b=(y+rows-y0)/dy;
      if (a>b) { float temp=a;a=b;b=temp; }
      if (a>0) first=(int)(a*steps);
      if (b<1) last=(int)(b*steps)+1;
    }
    for (int step=first;step<=last;step++) {
      float t=(float)step/steps;
      int x=(int)(x0+dx*t+.5f),yy=(int)(y0+dy*t+.5f)-y;
      if (x>=0 && x<width && yy>=0 && yy<rows) {
        unsigned at=(unsigned)(yy*width+x);
        if (out[at]!=VIEW_VOID) out[at]=line.color;
      }
    }
  }
  if (view->hide_link) {
    int x0=(int)(view->link[0]*width)-1,x1=(int)(view->link[2]*width)+1;
    int y0=(int)(view->link[1]*height)-1,y1=(int)(view->link[3]*height)+1;
    if (x0<0) x0=0;
    if (x1>=width) x1=width-1;
    for (int yy=0;yy<rows;yy++) if (y+yy>=y0 && y+yy<=y1 && x1>=x0)
      memset(out+(size_t)yy*width+x0,0,(size_t)(x1-x0+1));
  }
}

uint32_t void_view_color(uint32_t background,uint8_t color)
{
  static const uint8_t palette[VIEW_COLORS][3]={
    [VIEW_VOID]={255,64,40},[VIEW_BLOCKS]={59,214,255},[VIEW_INSIDE]={255,71,71},
    [VIEW_OTHER]={153,140,255},[VIEW_FLOOR]={77,255,89},[VIEW_WALL]={64,166,255},
    [VIEW_CEILING]={255,89,191},[VIEW_TRIGGER]={255,242,64}
  };
  if (!color || color>=VIEW_COLORS) return background;
  uint32_t result=background&0xff000000u;
  for (int c=0;c<3;c++) result|=(((background>>(8*c)&255)+palette[color][c]+1)/2)<<(8*c);
  return result;
}
