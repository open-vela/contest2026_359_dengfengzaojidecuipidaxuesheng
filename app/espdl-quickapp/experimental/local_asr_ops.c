/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr_ops.h"
#ifdef GLV_ASR_OFFICIAL_DSP
#include "local_asr_dsp.h"
#endif
#ifdef GLV_ASR_ACCEL_ENABLED
#include "local_asr_accel.h"
#endif
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>
#define TILE 128u

static bool product(size_t a,size_t b,size_t *out)
{if(b&&a>SIZE_MAX/b)return false;*out=a*b;return true;}
static bool overlap(const float *a,size_t na,const float *b,size_t nb)
{
  size_t ba,bb;if(!product(na,sizeof(float),&ba)||!product(nb,sizeof(float),&bb))return true;
  uintptr_t pa=(uintptr_t)a,pb=(uintptr_t)b;
  if(ba>UINTPTR_MAX-pa||bb>UINTPTR_MAX-pb)return true;
  return pa<pb+bb&&pb<pa+ba;
}
int glv_asr_matmul(struct glv_memory *memory,const float *x,size_t nx,float *y,size_t ny,
                  unsigned m,unsigned k,unsigned n,const struct glv_asr_rows *rows)
{
  size_t ax,ay,bytes;
  if(!memory||!x||!y||!rows||!rows->read||!m||!k||!n)return -EINVAL;
  if(!product(m,k,&ax)||!product(m,n,&ay)||!product(k,sizeof(float),&bytes))return -EOVERFLOW;
  if(ax>nx||ay>ny||overlap(x,ax,y,ay))return -EINVAL;
  int ret=glv_cancel(memory);if(ret)return ret;
#ifdef GLV_ASR_ACCEL_ENABLED
  if(!memory->asr_reference){ret=glv_asr_accel_matmul(memory,x,y,m,k,n,rows);if(ret!=1)return ret;ret=0;}
#endif
  float *weight=glv_alloc(memory,bytes);if(!weight)return -ENOMEM;
  for(unsigned o=0;o<n&&!ret;o++){
    ret=rows->read(rows->opaque,o,weight,k);if(ret)break;
    for(unsigned r=0;r<m;r++){
      float sum=0;const float *src=x+(size_t)r*k;
      for(unsigned i=0;i<k;i++)sum+=src[i]*weight[i];
      y[(size_t)r*n+o]=sum;
      if((r&31u)==31u&&(ret=glv_cancel(memory)))break;
    }
    if(!ret)ret=glv_cancel(memory);
  }
  glv_free(weight);return ret;
}
static int volume(const struct glv_asr_shape *shape,size_t *n)
{
  if(!shape||shape->rank>8)return -EINVAL;*n=1;
  for(unsigned i=0;i<shape->rank;i++)if(!product(*n,shape->dims[i],n))return -EOVERFLOW;
  return 0;
}
int glv_asr_unary(struct glv_memory *memory,enum glv_asr_unary op,const float *x,float *y,size_t n)
{
  if(!memory||(!x&&n)||(!y&&n)||op<GLV_ASR_NEG||op>GLV_ASR_RECIPROCAL)return -EINVAL;
  if(x!=y&&overlap(x,n,y,n))return -EINVAL;
  for(size_t i=0;i<n;i++){
    if(!(i&1023)){int ret=glv_cancel(memory);if(ret)return ret;}
    float v=x[i];
    switch(op){case GLV_ASR_NEG:v=-v;break;case GLV_ASR_RELU:v=v<0?0:v;break;
      case GLV_ASR_SIGMOID:v=1/(1+expf(-v));break;case GLV_ASR_TANH:v=tanhf(v);break;
      case GLV_ASR_RECIPROCAL:v=1/v;break;}
    y[i]=v;
  }return glv_cancel(memory);
}
int glv_asr_binary(struct glv_memory *memory,enum glv_asr_binary op,const float *x,size_t nx,const struct glv_asr_shape *sx,
                   const float *z,size_t nz,const struct glv_asr_shape *sz,float *y,size_t ny)
{
  size_t ax,az;int ret=volume(sx,&ax);if(ret)return ret;if((ret=volume(sz,&az)))return ret;
  if(!memory||(!x&&ax)||(!z&&az)||!y||nx<ax||nz<az||op<GLV_ASR_ADD||op>GLV_ASR_POW)return -EINVAL;
  unsigned rank=sx->rank>sz->rank?sx->rank:sz->rank;size_t count=1,dx[8],dz[8];unsigned dims[8];
  size_t a=1,b=1;
  for(unsigned r=rank;r>0;r--){
    unsigned i=r-1,xi=i+sx->rank,zi=i+sz->rank;
    unsigned vx=xi>=rank?sx->dims[xi-rank]:1,vz=zi>=rank?sz->dims[zi-rank]:1;
    if(vx!=vz&&vx!=1&&vz!=1)return -EINVAL;dims[i]=vx==1?vz:vx;
    dx[i]=vx==1?0:a;dz[i]=vz==1?0:b;
    if(!product(a,vx,&a)||!product(b,vz,&b)||!product(count,dims[i],&count))return -EOVERFLOW;
  }
  if(ny<count||(overlap(x,ax,y,count)&&!(x==y&&ax==count))||(overlap(z,az,y,count)&&!(z==y&&az==count)))return -EINVAL;
  if(!count)return glv_cancel(memory);
  /* Coalesce the maximal suffix where both operands are contiguous or
   * scalar. Broadcast coordinates then advance once per run, not per value.
   * Interior broadcast axes end the run, preserving e.g.[B,1,C]+[1,T,C]. */
  unsigned inner=rank,stepx=0,stepz=0;size_t span=1;bool selected=false;
  for(unsigned r=rank;r>0;r--){unsigned d=r-1;
    if(dims[d]==1){inner=d;continue;}
    if(!selected){if(dx[d]>1||dz[d]>1)break;stepx=(unsigned)dx[d];stepz=(unsigned)dz[d];selected=true;}
    else if(dx[d]!=(size_t)stepx*span||dz[d]!=(size_t)stepz*span)break;
    span*=dims[d];inner=d;
  }
  if(!memory->asr_reference&&span>=8){
    size_t coordinate[8]={0},ix=0,iz=0;
    for(size_t base=0;base<count;base+=span){
      for(size_t at=0;at<span;){unsigned n=span-at>1024?1024:(unsigned)(span-at);
        if((ret=glv_cancel(memory)))return ret;
        const float *a=x+ix+at*stepx,*b=z+iz+at*stepz;float *out=y+base+at;ret=1;
#ifdef GLV_ASR_OFFICIAL_DSP
        ret=glv_asr_dsp_binary(memory,op,a,b,out,n,stepx,stepz);
#endif
        if(ret<0)return ret;
        if(ret==1)for(unsigned i=0;i<n;i++){
          float u=a[i*stepx],v=b[i*stepz];
          switch(op){case GLV_ASR_ADD:out[i]=u+v;break;case GLV_ASR_SUB:out[i]=u-v;break;
            case GLV_ASR_MUL:out[i]=u*v;break;case GLV_ASR_DIV:out[i]=u/v;break;case GLV_ASR_POW:out[i]=powf(u,v);break;}
        }
        at+=n;
      }
      for(unsigned r=inner;r>0;r--){unsigned d=r-1;
        coordinate[d]++;ix+=dx[d];iz+=dz[d];if(coordinate[d]<dims[d])break;
        coordinate[d]=0;ix-=dx[d]*dims[d];iz-=dz[d]*dims[d];
      }
    }
    return glv_cancel(memory);
  }
  size_t coordinate[8]={0},ix=0,iz=0;
  for(size_t i=0;i<count;i++){
    if(!(i&1023)&&(ret=glv_cancel(memory)))return ret;
    float u=x[ix],v=z[iz],result=0;
    switch(op){case GLV_ASR_ADD:result=u+v;break;case GLV_ASR_SUB:result=u-v;break;
      case GLV_ASR_MUL:result=u*v;break;case GLV_ASR_DIV:result=u/v;break;case GLV_ASR_POW:result=powf(u,v);break;}
    y[i]=result;
    for(unsigned r=rank;r>0;r--){unsigned d=r-1;
      coordinate[d]++;ix+=dx[d];iz+=dz[d];
      if(coordinate[d]<dims[d])break;
      coordinate[d]=0;ix-=dx[d]*dims[d];iz-=dz[d]*dims[d];
    }
  }return glv_cancel(memory);
}
int glv_asr_softmax(struct glv_memory *memory,const float *x,size_t nx,const struct glv_asr_shape *shape,unsigned axis,float *y,size_t ny)
{
  size_t n;int ret=volume(shape,&n);if(ret)return ret;
  if(!memory||!x||!y||axis>=shape->rank||nx<n||ny<n||overlap(x,n,y,n))return -EINVAL;
  if(!n)return glv_cancel(memory);size_t outer=1,inner=1;unsigned width=shape->dims[axis];
  for(unsigned i=0;i<axis;i++)outer*=shape->dims[i];for(unsigned i=axis+1;i<shape->rank;i++)inner*=shape->dims[i];
  for(size_t a=0;a<outer;a++)for(size_t b=0;b<inner;b++){
    if((ret=glv_cancel(memory)))return ret;size_t base=a*width*inner+b;float maximum=-INFINITY,total=0;
    for(unsigned c=0;c<width;c++)if(x[base+c*inner]>maximum)maximum=x[base+c*inner];
    for(unsigned c=0;c<width;c++){float v=expf(x[base+c*inner]-maximum);y[base+c*inner]=v;total+=v;}
    for(unsigned c=0;c<width;c++)y[base+c*inner]/=total;
  }return 0;
}
int glv_asr_reduce(struct glv_memory *memory,const float *x,size_t nx,const struct glv_asr_shape *shape,unsigned axes,bool mean,float *y,size_t ny)
{
  size_t n;int ret=volume(shape,&n);if(ret)return ret;
  if(!memory||(!x&&n)||!y||nx<n||axes>=(1u<<shape->rank))return -EINVAL;
  size_t count=1,reduced=1;for(unsigned i=0;i<shape->rank;i++){
    size_t *p=axes&(1u<<i)?&reduced:&count;if(!product(*p,shape->dims[i],p))return -EOVERFLOW;
  }
  if(ny<count||overlap(x,n,y,count))return -EINVAL;
  for(size_t i=0;i<count;i++)y[i]=0;
  for(size_t i=0;i<n;i++){
    if(!(i&1023)&&(ret=glv_cancel(memory)))return ret;
    size_t q=i,index=0,stride=1;
    for(unsigned r=shape->rank;r>0;r--){unsigned d=r-1;size_t c=q%shape->dims[d];q/=shape->dims[d];if(!(axes&(1u<<d))){index+=c*stride;stride*=shape->dims[d];}}
    y[index]+=x[i];
  }
  if(mean)for(size_t i=0;i<count;i++)y[i]/=(float)reduced;
  return glv_cancel(memory);
}
int glv_asr_gemm(struct glv_memory *memory,const float *x,size_t nx,float *y,size_t ny,unsigned m,unsigned k,unsigned n,
                 const struct glv_asr_rows *rows,float alpha,float beta,const float *bias,size_t nbias,const struct glv_asr_shape *shape)
{
  size_t count,bc=0;if(!m||!n||!product(m,n,&count))return -EINVAL;
  if(bias){
    int ret=volume(shape,&bc);if(ret)return ret;
    if(shape->rank>2||nbias<bc||overlap(bias,bc,y,count))return -EINVAL;
    if(shape->rank==1&&shape->dims[0]!=1&&shape->dims[0]!=n)return -EINVAL;
    if(shape->rank==2&&((shape->dims[0]!=1&&shape->dims[0]!=m)||(shape->dims[1]!=1&&shape->dims[1]!=n)))return -EINVAL;
  }
  int ret=glv_asr_matmul(memory,x,nx,y,ny,m,k,n,rows);if(ret)return ret;
  for(size_t i=0;i<count;i++){
    if(!(i&1023)&&(ret=glv_cancel(memory)))return ret;
    float value=alpha==1?y[i]:alpha*y[i];
    if(bias&&beta!=0){size_t at=0;
      if(shape->rank==1)at=shape->dims[0]==1?0:i%n;
      else if(shape->rank==2)at=(shape->dims[0]==1?0:i/n)*shape->dims[1]+(shape->dims[1]==1?0:i%n);
      value+=beta==1?bias[at]:beta*bias[at];
    }y[i]=value;
  }return 0;
}
int glv_asr_conv(struct glv_memory *memory,const float *x,size_t nx,const float *bias,size_t nbias,
                float *y,size_t ny,const struct glv_asr_conv *c,const struct glv_asr_rows *rows)
{
  if(!memory||!x||!y||!c||!rows||!rows->read||!c->channels||!c->height||!c->width||
     !c->outputs||!c->kernel_h||!c->kernel_w||!c->groups||!c->stride_h||!c->stride_w||
     !c->dilation_h||!c->dilation_w||c->channels%c->groups||c->outputs%c->groups)return -EINVAL;
  uint64_t kh=(uint64_t)c->dilation_h*(c->kernel_h-1)+1,kw=(uint64_t)c->dilation_w*(c->kernel_w-1)+1;
  uint64_t hp=(uint64_t)c->height+c->top+c->bottom,wp=(uint64_t)c->width+c->left+c->right;
  if(kh>hp||kw>wp)return -EINVAL;
  uint64_t height=(hp-kh)/c->stride_h+1,width=(wp-kw)/c->stride_w+1;
  if(height>INT_MAX||width>INT_MAX||hp>INT_MAX||wp>INT_MAX)return -EOVERFLOW;
  size_t plane,ax,output_plane,ay,count,bytes;
  unsigned ic=c->channels/c->groups,oc=c->outputs/c->groups;
  if(!product(c->height,c->width,&plane)||!product(plane,c->channels,&ax)||!product(height,width,&output_plane)||
     !product(output_plane,c->outputs,&ay)||!product(ic,c->kernel_h,&count)||!product(count,c->kernel_w,&count)||
     count>UINT_MAX||!product(count,sizeof(float),&bytes))return -EOVERFLOW;
  if(ax>nx||ay>ny||overlap(x,ax,y,ay)||(bias&&(nbias<c->outputs||overlap(bias,c->outputs,y,ay))))return -EINVAL;
  int ret=glv_cancel(memory);if(ret)return ret;
#ifdef GLV_ASR_ACCEL_ENABLED
  if(!memory->asr_reference){ret=glv_asr_accel_conv(memory,x,bias,y,c,rows,(unsigned)count,(unsigned)height,(unsigned)width);if(ret!=1)return ret;ret=0;}
#endif
  float *weight=glv_alloc(memory,bytes);if(!weight)return -ENOMEM;
  for(unsigned o=0;o<c->outputs&&!ret;o++){
    ret=rows->read(rows->opaque,o,weight,(unsigned)count);if(ret)break;
    for(size_t at=0;at<output_plane&&!ret;at+=TILE){
      size_t n=output_plane-at;if(n>TILE)n=TILE;float sums[TILE];int ys[TILE],xs[TILE];
      for(unsigned t=0;t<n;t++){
        size_t pos=at+t;sums[t]=bias?bias[o]:0;
        ys[t]=(int)(pos/(unsigned)width*c->stride_h)-(int)c->top;
        xs[t]=(int)(pos%(unsigned)width*c->stride_w)-(int)c->left;
      }
      for(unsigned i=0;i<ic;i++)for(unsigned h=0;h<c->kernel_h;h++)for(unsigned w=0;w<c->kernel_w;w++){
        float value=weight[((size_t)i*c->kernel_h+h)*c->kernel_w+w];
        const float *src=x+((size_t)(o/oc)*ic+i)*plane;
        for(unsigned t=0;t<n;t++){
          int y0=ys[t]+(int)(h*c->dilation_h),x0=xs[t]+(int)(w*c->dilation_w);
          if(y0>=0&&(unsigned)y0<c->height&&x0>=0&&(unsigned)x0<c->width)sums[t]+=src[(size_t)y0*c->width+x0]*value;
        }
      }
      for(unsigned t=0;t<n;t++)y[(size_t)o*output_plane+at+t]=sums[t];
      ret=glv_cancel(memory);
    }
  }
  glv_free(weight);return ret;
}
