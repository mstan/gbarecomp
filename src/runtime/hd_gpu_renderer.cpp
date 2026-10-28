#include "hd_gpu_renderer.h"
#include "color_lut.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
#include "gba_native_draw_frame.h"
#include "native_gpu_shader.h"
#endif

#if defined(GBARECOMP_HAVE_SDL2) && !defined(__ANDROID__) && !defined(GBARECOMP_WEB_HOST)
#include <SDL.h>
#if SDL_VERSION_ATLEAST(2,0,10)
#define GBARECOMP_HAVE_GPU_HD_API 1
#include <SDL_opengl.h>
#endif
#endif

#if defined(GBARECOMP_HAVE_GPU_HD_API)

namespace gbarecomp {
namespace {
using Hd = gba::GbaHighResolutionFrame;
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
constexpr const char* native_grading_source = R"GLSL(#version 130
uniform sampler2D source_plane;
uniform sampler2D grading;
out vec4 result;
void main() {
    ivec3 rgb=ivec3(floor(texelFetch(source_plane,ivec2(gl_FragCoord.xy),0).rgb*255.0+0.5));
    int index=(rgb.x>>3)|((rgb.y>>3)<<5)|((rgb.z>>3)<<10);
    result=vec4(texelFetch(grading,ivec2(index&255,index>>8),0).rgb,1.0);
}
)GLSL";
#endif
constexpr const char* vertex_source = R"GLSL(#version 130
void main() {
    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)GLSL";
constexpr const char* fragment_source = R"GLSL(#version 130
uniform isampler2D top_plane, second_plane, metadata;
uniform usampler2D palette_plane, surface0, surface1;
uniform sampler2D grading;
uniform ivec2 source_size, output_size;
uniform int scale, graded;
out vec4 result;
struct Pixel { uint color; int key, layer; bool t1,t2,valid; };
Pixel pixel(ivec4 data) {
    int f=data.z;
    return Pixel(uint(data.x),data.y,f&7,(f&8)!=0,(f&16)!=0,(f&32)!=0);
}
void insert_pixel(inout Pixel top,inout Pixel second,Pixel candidate) {
    if(candidate.key<top.key) { second=top; top=candidate; }
    else if(candidate.key<second.key) second=candidate;
}
uint index_at(int surface,ivec2 point) {
    if(surface==0) return texelFetch(surface0,point,0).r;
    return texelFetch(surface1,point,0).r;
}
ivec3 channels(uint color) {
    return ivec3(int(color&31u),int((color>>5u)&31u),int((color>>10u)&31u));
}
uint pack_color(ivec3 c) { return uint(c.x|(c.y<<5)|(c.z<<10)); }
int divide_zero(int numerator,int denominator) {
    // All intermediate magnitudes are proven below INT_MAX by pack(). Make
    // CPU truncation explicit instead of relying on legacy signed division.
    return numerator<0 ? -((-numerator)/denominator) : numerator/denominator;
}
bool sample_color(int surface,int cnt,int u,int v,int row,bool filtering,out uint color) {
    int size=128<<((cnt>>14)&3);
    // Explicit floor keeps negative fixed-point coordinates independent of
    // a driver's signed right-shift lowering.
    int x=u>=0 ? u/256 : -((-u+255)/256);
    int y=v>=0 ? v/256 : -((-v+255)/256);
    bool wrap=(cnt&8192)!=0;
    if(wrap) { x&=size-1; y&=size-1; }
    else if(x<0 || y<0 || x>=size || y>=size) return false;
    uint c00=index_at(surface,ivec2(x,y));
    if(c00==0u) return false;
    color=texelFetch(palette_plane,ivec2(int(c00),row),0).r;
    if(!filtering) return true;
    int x1=x+1,y1=y+1;
    if(wrap) { x1&=size-1; y1&=size-1; }
    else if(x1==size || y1==size) return true;
    uint c10=index_at(surface,ivec2(x1,y));
    uint c01=index_at(surface,ivec2(x,y1));
    uint c11=index_at(surface,ivec2(x1,y1));
    if(c10==0u || c01==0u || c11==0u) return true;
    int fx=u&255,fy=v&255;
    ivec3 sum=channels(color)*((256-fx)*(256-fy))
        +channels(texelFetch(palette_plane,ivec2(int(c10),row),0).r)*(fx*(256-fy))
        +channels(texelFetch(palette_plane,ivec2(int(c01),row),0).r)*((256-fx)*fy)
        +channels(texelFetch(palette_plane,ivec2(int(c11),row),0).r)*(fx*fy);
    color=pack_color((sum+ivec3(32768))>>16);
    return true;
}
uint blend(uint a,uint b,int eva,int evb) {
    eva=min(eva,16);evb=min(evb,16);
    int r=min(31,(int(a&31u)*eva+int(b&31u)*evb+8)>>4);
    int ag=int(((a>>4u)&62u)|(a>>15u)),bg=int(((b>>4u)&62u)|(b>>15u));
    int g=min(63,(ag*eva+bg*evb+8)>>4);
    int blue=min(31,(int((a>>10u)&31u)*eva+int((b>>10u)&31u)*evb+8)>>4);
    return uint(r|((g>>1)<<5)|(blue<<10));
}
uint brightness(uint c,int value,bool light) {
    value=min(value,16);
    ivec3 v=ivec3(int(c&31u),int(((c>>4u)&62u)|(c>>15u)),int((c>>10u)&31u));
    if(light) v+=((ivec3(31,63,31)-v)*value+ivec3(8))>>4;
    else v-=(v*value+ivec3(7))>>4;
    return uint(v.x|((v.y>>1)<<5)|(v.z<<10));
}
void output_rgb(ivec3 rgb) {
    if(graded!=0) {
        int index=(rgb.x>>3)|((rgb.y>>3)<<5)|((rgb.z>>3)<<10);
        result=vec4(texelFetch(grading,ivec2(index&255,index>>8),0).rgb,1.0);
    } else result=vec4(vec3(rgb)/255.0,1.0);
}
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    if(any(notEqual(source_size,output_size))) {
        // Exact integer rescaling without the full coordinate*source product.
        // At the supported maximum source dimension (1280*64), each remaining
        // product fits signed 32 bits, including a texture-limited fallback.
        p=p*(source_size/output_size)+p*(source_size%output_size)/output_size;
    }
    ivec2 native=p/scale;
    int sx=p.x%scale,sy=p.y%scale;
    ivec4 config=texelFetch(metadata,ivec2(0,native.y),0);
    ivec4 extra=texelFetch(metadata,ivec2(1,native.y),0);
    ivec4 top_data=texelFetch(top_plane,native,0);
    if(config.x==2) {
        int rgb=top_data.w;
        output_rgb(ivec3(rgb&255,(rgb>>8)&255,(rgb>>16)&255));return;
    }
    if((top_data.z&16384)!=0) { output_rgb(ivec3(0));return; }
    Pixel top=pixel(top_data),second=pixel(texelFetch(second_plane,native,0));
    int window=(top_data.z>>8)&63;
    for(int i=0;i<2;++i) {
        ivec4 a=texelFetch(metadata,ivec2(2+i*3,native.y),0);
        int layer=2+i;
        if(a.x==0 || (window&(1<<layer))==0) continue;
        ivec4 step=texelFetch(metadata,ivec2(3+i*3,native.y),0);
        ivec4 delta=texelFetch(metadata,ivec2(4+i*3,native.y),0);
        int pa=step.x+divide_zero(delta.x*sy,scale),pc=step.y+divide_zero(delta.y*sy,scale);
        int u=a.z+divide_zero(step.z*sy,scale)-config.y*pa+native.x*pa+divide_zero(pa*sx,scale);
        int v=a.w+divide_zero(step.w*sy,scale)-config.y*pc+native.x*pc+divide_zero(pc*sx,scale);
        uint color;
        if(!sample_color(i,a.y,u,v,native.y,extra.y!=0,color)) continue;
        insert_pixel(top,second,Pixel(color,(a.y&3)*256+130+i,layer,
            (window&32)!=0 && (config.z&(1<<layer))!=0,
            (config.z&(256<<layer))!=0,true));
    }
    int effect=(config.z>>6)&3;
    uint color=top.color;
    if((effect==1 || top.layer==4) && top.t1 && second.valid && second.t2 &&
        !(top.layer==4 && second.layer==4)) color=blend(color,second.color,config.w&31,(config.w>>8)&31);
    else if(top.t1 && effect==2) color=brightness(color,extra.x,true);
    else if(top.t1 && effect==3) color=brightness(color,extra.x,false);
    ivec3 c=channels(color);
    output_rgb((c<<3)|(c>>2));
}
)GLSL";

#define GL_FUNCTIONS(X) \
    X(const GLubyte*,GetString,(GLenum)) \
    X(void,GetIntegerv,(GLenum,GLint*)) \
    X(void,GetBooleanv,(GLenum,GLboolean*)) \
    X(GLboolean,IsEnabled,(GLenum)) \
    X(GLenum,GetError,()) \
    X(void,Enable,(GLenum)) X(void,Disable,(GLenum)) \
    X(void,Viewport,(GLint,GLint,GLsizei,GLsizei)) X(void,Scissor,(GLint,GLint,GLsizei,GLsizei)) \
    X(void,ColorMask,(GLboolean,GLboolean,GLboolean,GLboolean)) \
    X(void,PixelStorei,(GLenum,GLint)) X(void,ActiveTexture,(GLenum)) \
    X(void,GenTextures,(GLsizei,GLuint*)) X(void,DeleteTextures,(GLsizei,const GLuint*)) \
    X(void,BindTexture,(GLenum,GLuint)) X(void,TexParameteri,(GLenum,GLenum,GLint)) \
    X(void,TexImage2D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*)) \
    X(void,TexSubImage2D,(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*)) \
    X(GLuint,CreateShader,(GLenum)) X(void,ShaderSource,(GLuint,GLsizei,const GLchar*const*,const GLint*)) \
    X(void,CompileShader,(GLuint)) X(void,GetShaderiv,(GLuint,GLenum,GLint*)) \
    X(void,GetShaderInfoLog,(GLuint,GLsizei,GLsizei*,GLchar*)) X(void,DeleteShader,(GLuint)) \
    X(GLuint,CreateProgram,()) X(void,AttachShader,(GLuint,GLuint)) X(void,LinkProgram,(GLuint)) \
    X(void,BindFragDataLocation,(GLuint,GLuint,const GLchar*)) \
    X(void,GetProgramiv,(GLuint,GLenum,GLint*)) X(void,GetProgramInfoLog,(GLuint,GLsizei,GLsizei*,GLchar*)) \
    X(void,UseProgram,(GLuint)) X(void,DeleteProgram,(GLuint)) \
    X(GLint,GetUniformLocation,(GLuint,const GLchar*)) \
    X(void,Uniform1i,(GLint,GLint)) X(void,Uniform2i,(GLint,GLint,GLint)) \
    X(void,GenVertexArrays,(GLsizei,GLuint*)) X(void,DeleteVertexArrays,(GLsizei,const GLuint*)) \
    X(void,BindVertexArray,(GLuint)) X(void,BindBuffer,(GLenum,GLuint)) \
    X(void,DrawArrays,(GLenum,GLint,GLsizei))
struct Gl {
#define DECLARE(ret,name,args) using name##Fn=ret(APIENTRY*)args;name##Fn name{};
    GL_FUNCTIONS(DECLARE)
#undef DECLARE
    bool load() {
#define LOAD(ret,name,args) name=reinterpret_cast<name##Fn>(SDL_GL_GetProcAddress("gl" #name));if(!name)return false;
        GL_FUNCTIONS(LOAD)
#undef LOAD
        return true;
    }
};
// SDL caches its own GL state. Restore every state modified here before giving
// the context back, in addition to restoring its public render-target contract.
struct State {
    Gl& g;
    GLint program{},active{},vao{},unpack_buffer{},viewport[4]{},scissor[4]{};
    GLint textures[7]{},unpack[6]{};
    GLboolean mask[4]{},enabled[8]{};
    static constexpr GLenum switches[]={GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_STENCIL_TEST,
        GL_CULL_FACE,GL_DITHER,GL_FRAMEBUFFER_SRGB,GL_RASTERIZER_DISCARD};
    static constexpr GLenum stores[]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_PIXELS,
        GL_UNPACK_SKIP_ROWS,GL_UNPACK_SWAP_BYTES,GL_UNPACK_LSB_FIRST};
    explicit State(Gl& gl):g(gl) {
        g.GetIntegerv(GL_CURRENT_PROGRAM,&program);g.GetIntegerv(GL_ACTIVE_TEXTURE,&active);
        g.GetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);g.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack_buffer);
        g.GetIntegerv(GL_VIEWPORT,viewport);g.GetIntegerv(GL_SCISSOR_BOX,scissor);
        g.GetBooleanv(GL_COLOR_WRITEMASK,mask);
        for(unsigned i=0;i<8;++i)enabled[i]=g.IsEnabled(switches[i]);
        for(unsigned i=0;i<6;++i)g.GetIntegerv(stores[i],&unpack[i]);
        for(unsigned i=0;i<7;++i) {g.ActiveTexture(GL_TEXTURE0+i);g.GetIntegerv(GL_TEXTURE_BINDING_2D,&textures[i]);}
        g.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        g.PixelStorei(GL_UNPACK_ALIGNMENT,1);g.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
        g.PixelStorei(GL_UNPACK_SKIP_PIXELS,0);g.PixelStorei(GL_UNPACK_SKIP_ROWS,0);
        g.PixelStorei(GL_UNPACK_SWAP_BYTES,GL_FALSE);g.PixelStorei(GL_UNPACK_LSB_FIRST,GL_FALSE);
        for(GLenum name:switches)g.Disable(name);
        g.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    }
    ~State() {
        g.UseProgram(GLuint(program));g.BindVertexArray(GLuint(vao));
        g.BindBuffer(GL_PIXEL_UNPACK_BUFFER,GLuint(unpack_buffer));
        for(unsigned i=0;i<6;++i)g.PixelStorei(stores[i],unpack[i]);
        for(unsigned i=0;i<7;++i){g.ActiveTexture(GL_TEXTURE0+i);g.BindTexture(GL_TEXTURE_2D,GLuint(textures[i]));}
        g.ActiveTexture(GLenum(active));g.Viewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        g.Scissor(scissor[0],scissor[1],scissor[2],scissor[3]);g.ColorMask(mask[0],mask[1],mask[2],mask[3]);
        for(unsigned i=0;i<8;++i){if(enabled[i])g.Enable(switches[i]);else g.Disable(switches[i]);}
    }
};
int pixel_flags(const Hd::Pixel& p) {
    return p.layer | (int(p.target1)<<3) | (int(p.target2)<<4) | (int(p.valid)<<5);
}
bool within(int64_t n,int64_t limit) {return n>=-limit && n<=limit;}
} // namespace

struct HdGpuRenderer::Impl {
    Gl gl;
    SDL_Renderer* renderer{};
    SDL_GLContext context{};
    SDL_Texture* output{};
    unsigned width{},height{},max_texture{};
    unsigned requested_w{},requested_h{};
    GLuint program{},vao{},textures[7]{};
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
    GLuint native_program{};
    GLuint native_grading_program{};
    SDL_Texture* native_graded{};
    SDL_Texture* last_output{};
    bool native_output=false;
    bool native_planes=false,native_failed=false;
    uint64_t native_frames=0,native_uploaded_bytes=0;
    std::vector<uint8_t> native_raster;
#endif
    unsigned plane_w{},plane_h{};
    bool failed=false,lost=false;
    uint64_t frame_serial=0,frames=0,uploaded_surfaces=0;
    struct Cached {
        std::weak_ptr<const Hd::IndexedSurface> source;
        GLuint texture=0;
        uint64_t last=0;
    };
    std::unordered_map<const Hd::IndexedSurface*,Cached> surfaces;
    std::vector<GLint> top,second,meta;
    std::vector<uint16_t> palette;
    ~Impl() {
        if(!lost && context && SDL_GL_GetCurrentContext()==context) {
            for(auto& entry:surfaces)gl.DeleteTextures(1,&entry.second.texture);
            gl.DeleteTextures(7,textures);
            if(vao)gl.DeleteVertexArrays(1,&vao);
            if(program)gl.DeleteProgram(program);
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
            if(native_program)gl.DeleteProgram(native_program);
            if(native_grading_program)gl.DeleteProgram(native_grading_program);
#endif
        }
        if(output)SDL_DestroyTexture(output);
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
        if(native_graded)SDL_DestroyTexture(native_graded);
#endif
        if(frames)std::fprintf(stderr,"hd_gpu: frames=%llu surface_uploads=%llu\n",
            (unsigned long long)frames,(unsigned long long)uploaded_surfaces);
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
        if(native_frames)std::fprintf(stderr,"native_gpu: frames=%llu vram_uploaded_bytes=%llu\n",
            (unsigned long long)native_frames,(unsigned long long)native_uploaded_bytes);
#endif
    }
    GLuint shader(GLenum type,const char* source) {
        GLuint value=gl.CreateShader(type);gl.ShaderSource(value,1,&source,nullptr);gl.CompileShader(value);
        GLint ok=0;gl.GetShaderiv(value,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[4096]{};gl.GetShaderInfoLog(value,sizeof(log),nullptr,log);
            std::fprintf(stderr,"hd_gpu: shader unavailable: %s\n",log);gl.DeleteShader(value);return 0;}
        return value;
    }
    void bind(unsigned unit,GLuint texture) {gl.ActiveTexture(GL_TEXTURE0+unit);gl.BindTexture(GL_TEXTURE_2D,texture);}
    void image(unsigned unit,GLint internal,unsigned w,unsigned h,GLenum format,GLenum type,const void* data) {
        bind(unit,textures[unit]);
        gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D,0,internal,GLsizei(w),GLsizei(h),0,format,type,data);
    }
    void upload(unsigned unit,unsigned w,unsigned h,GLenum format,GLenum type,const void* data) {
        bind(unit,textures[unit]);gl.TexSubImage2D(GL_TEXTURE_2D,0,0,0,GLsizei(w),GLsizei(h),format,type,data);
    }
    GLuint surface(const std::shared_ptr<const Hd::IndexedSurface>& source) {
        if(!source)return textures[4]; // zero-index dummy, used only for inactive layers
        auto found=surfaces.find(source.get());
        if(found!=surfaces.end() && found->second.source.expired()) {
            gl.DeleteTextures(1,&found->second.texture);surfaces.erase(found);found=surfaces.end();
        }
        if(found==surfaces.end()) {
            Cached value;value.source=source;gl.GenTextures(1,&value.texture);
            bind(4,value.texture);gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            gl.TexImage2D(GL_TEXTURE_2D,0,GL_R8UI,GLsizei(source->size),GLsizei(source->size),0,
                GL_RED_INTEGER,GL_UNSIGNED_BYTE,source->texels.data());
            found=surfaces.emplace(source.get(),std::move(value)).first;++uploaded_surfaces;
        }
        found->second.last=frame_serial;return found->second.texture;
    }
    bool pack(const Hd::DrawFrame& frame) {
        if(frame.width<240 || frame.width>1280 || frame.height<160 || frame.height>864 ||
            frame.scale<2 || frame.scale>Hd::kMaxScale || frame.lines.size()!=frame.height ||
            frame.width>max_texture || frame.height>max_texture)return false;
        top.assign(size_t(frame.width)*frame.height*4,0);second.assign(top.size(),0);
        meta.assign(size_t(frame.height)*8*4,0);palette.assign(size_t(frame.height)*256,0);
        // The shader multiplies each vertical delta by sy before dividing by
        // scale. Preserve the original <=16x admission and bound that product
        // at the new scales; broader deltas still use the portable renderer.
        const int64_t delta_limit=std::min<int64_t>(1ll<<27,INT_MAX/(frame.scale-1));
        for(unsigned y=0;y<frame.height;++y) {
            const auto& line=frame.lines[y];
            auto* m=meta.data()+size_t(y)*32;
            m[0]=line.kind;m[1]=GLint(line.ox);m[2]=line.bldcnt;m[3]=line.alpha;
            m[4]=GLint(line.brightness);m[5]=line.filter;
            if(line.kind==Hd::LineJob::UpscaleLine) {
                if(line.native_rgb.size()!=size_t(frame.width)*3)return false;
                for(unsigned x=0;x<frame.width;++x){auto* t=top.data()+(size_t(y)*frame.width+x)*4;
                    const auto* c=line.native_rgb.data()+x*3;t[3]=c[0]|(int(c[1])<<8)|(int(c[2])<<16);}
                continue;
            }
            if(line.kind!=Hd::LineJob::AffineLine || line.ox>frame.width || line.brightness>16 ||
                line.top.size()!=frame.width || line.second.size()!=frame.width ||
                line.windows.size()!=frame.width || line.black.size()!=frame.width)return false;
            std::copy(line.palette.begin(),line.palette.end(),palette.begin()+size_t(y)*256);
            for(unsigned x=0;x<frame.width;++x) {
                const auto& p=line.top[x];const auto& q=line.second[x];
                if(p.layer>5 || q.layer>5)return false;
                auto* t=top.data()+(size_t(y)*frame.width+x)*4;auto* s=second.data()+(size_t(y)*frame.width+x)*4;
                t[0]=p.color;t[1]=p.key;t[2]=pixel_flags(p)|((line.windows[x]&63)<<8)|(!!line.black[x]<<14);
                s[0]=q.color;s[1]=q.key;s[2]=pixel_flags(q);
            }
            for(unsigned i=0;i<2;++i) {
                const auto& a=line.affine[i];auto* p=m+8+i*12;
                if(!a.active)continue;
                const auto& s=line.surfaces[i];
                // These bounds prove every signed32 intermediate in the shader.
                // Broader synthetic inputs remain supported by the CPU renderer.
                if(a.cache!=i || !s || s->size!=(128u<<(a.cnt>>14)) || s->size>max_texture ||
                    s->texels.size()!=size_t(s->size)*s->size || !within(a.x,1ll<<29) || !within(a.v,1ll<<29) ||
                    !within(a.pa,32768) || !within(a.pc,32768) || !within(a.dx,delta_limit) || !within(a.dv,delta_limit) ||
                    !within(a.dpa,65535) || !within(a.dpc,65535))return false;
                p[0]=1;p[1]=a.cnt;p[2]=GLint(a.x);p[3]=GLint(a.v);
                p[4]=GLint(a.pa);p[5]=GLint(a.pc);p[6]=GLint(a.dx);p[7]=GLint(a.dv);
                p[8]=GLint(a.dpa);p[9]=GLint(a.dpc);
            }
        }
        return true;
    }
};

HdGpuRenderer::HdGpuRenderer()=default;
HdGpuRenderer::~HdGpuRenderer(){reset();}
bool HdGpuRenderer::available()const{return impl_ && !impl_->failed;}
bool HdGpuRenderer::initialize(SDL_Renderer* renderer,SDL_Texture* activation) {
    if(impl_)return !impl_->failed;
    SDL_RendererInfo info{};
    if(!renderer || !activation || SDL_GetRendererInfo(renderer,&info)!=0 || !info.name ||
        std::strcmp(info.name,"opengl") || !(info.flags&SDL_RENDERER_TARGETTEXTURE))return false;
    if(SDL_GL_BindTexture(activation,nullptr,nullptr)!=0)return false;
    SDL_GL_UnbindTexture(activation);
    auto state=std::make_unique<Impl>();state->renderer=renderer;
    // Leave the destructor inert until the complete required API is available.
    if(!state->gl.load())return false;
    state->context=SDL_GL_GetCurrentContext();
    const auto* version=state->gl.GetString(GL_VERSION);int major=0;
    if(!version || std::sscanf(reinterpret_cast<const char*>(version),"%d",&major)!=1 || major<3)return false;
    GLint units=0,limit=0;state->gl.GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&units);
    state->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);
    if(units<7 || limit<1024)return false;
    state->max_texture=unsigned(limit);
    GLuint vertex=state->shader(GL_VERTEX_SHADER,vertex_source);
    GLuint fragment=state->shader(GL_FRAGMENT_SHADER,fragment_source);
    if(!vertex || !fragment){if(vertex)state->gl.DeleteShader(vertex);if(fragment)state->gl.DeleteShader(fragment);return false;}
    state->program=state->gl.CreateProgram();state->gl.AttachShader(state->program,vertex);
    state->gl.AttachShader(state->program,fragment);
    state->gl.BindFragDataLocation(state->program,0,"result");state->gl.LinkProgram(state->program);
    state->gl.DeleteShader(vertex);state->gl.DeleteShader(fragment);
    GLint linked=0;state->gl.GetProgramiv(state->program,GL_LINK_STATUS,&linked);
    if(!linked){char log[4096]{};state->gl.GetProgramInfoLog(state->program,sizeof(log),nullptr,log);
        std::fprintf(stderr,"hd_gpu: link unavailable: %s\n",log);return false;}
    state->gl.GenTextures(7,state->textures);state->gl.GenVertexArrays(1,&state->vao);
    {
        State restore(state->gl);uint8_t zero=0;
        state->image(4,GL_R8UI,1,1,GL_RED_INTEGER,GL_UNSIGNED_BYTE,&zero);
        state->image(5,GL_R8UI,1,1,GL_RED_INTEGER,GL_UNSIGNED_BYTE,&zero);
        state->image(6,GL_RGB8,256,128,GL_RGB,GL_UNSIGNED_BYTE,nullptr);
    }
    if(const GLenum error=state->gl.GetError();error!=GL_NO_ERROR){
        std::fprintf(stderr,"hd_gpu: initialization GL error=0x%x\n",unsigned(error));return false;
    }
    std::fprintf(stderr,"hd_gpu: available GL=%s renderer=%s limit=%u\n",version,
        state->gl.GetString(GL_RENDERER),state->max_texture);
    impl_=std::move(state);return true;
}
SDL_Texture* HdGpuRenderer::render(const Hd::DrawFrame& frame,const runtime::ColorLut* lut) {
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
    if(impl_) { impl_->native_output=false; impl_->last_output=nullptr; }
#endif
    if(!impl_ || impl_->failed)return nullptr;
    auto& s=*impl_;
    if(!s.pack(frame))return nullptr;
    unsigned width=frame.width*frame.scale,height=frame.height*frame.scale;
    SDL_RendererInfo info{};SDL_GetRendererInfo(s.renderer,&info);
    const auto max_w=std::min(s.max_texture,info.max_texture_width>0?unsigned(info.max_texture_width):s.max_texture);
    const auto max_h=std::min(s.max_texture,info.max_texture_height>0?unsigned(info.max_texture_height):s.max_texture);
    while(width>max_w || height>max_h){width=std::max(1u,width/2);height=std::max(1u,height/2);}
    if(!s.output || s.requested_w!=frame.width*frame.scale || s.requested_h!=frame.height*frame.scale) {
        SDL_Texture* replacement=nullptr;
        while(!replacement) {
            replacement=SDL_CreateTexture(s.renderer,SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET,int(width),int(height));
            if(replacement)break;
            if(width==1 && height==1){s.failed=true;return nullptr;}
            width=std::max(1u,width/2);height=std::max(1u,height/2);
        }
        SDL_SetTextureBlendMode(replacement,SDL_BLENDMODE_NONE);
        if(s.output)SDL_DestroyTexture(s.output);s.output=replacement;s.width=width;s.height=height;
        s.requested_w=frame.width*frame.scale;s.requested_h=frame.height*frame.scale;
        std::fprintf(stderr,"hd_gpu: target requested=%ux%u allocated=%ux%u scale=%u\n",
            s.requested_w,s.requested_h,s.width,s.height,frame.scale);
    } else {width=s.width;height=s.height;}
    SDL_Texture* old_target=SDL_GetRenderTarget(s.renderer);
    if(SDL_SetRenderTarget(s.renderer,s.output)!=0)return nullptr;
    if(SDL_RenderFlush(s.renderer)!=0){SDL_SetRenderTarget(s.renderer,old_target);return nullptr;}
    if(SDL_GL_GetCurrentContext()!=s.context){SDL_SetRenderTarget(s.renderer,old_target);s.failed=true;return nullptr;}
    bool good=true;
    {
        State restore(s.gl);++s.frame_serial;
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
        if(s.native_planes){s.plane_w=s.plane_h=0;s.native_planes=false;}
#endif
        if(s.plane_w!=frame.width || s.plane_h!=frame.height) {
            s.image(0,GL_RGBA32I,frame.width,frame.height,GL_RGBA_INTEGER,GL_INT,s.top.data());
            s.image(1,GL_RGBA32I,frame.width,frame.height,GL_RGBA_INTEGER,GL_INT,s.second.data());
            s.image(2,GL_RGBA32I,8,frame.height,GL_RGBA_INTEGER,GL_INT,s.meta.data());
            s.image(3,GL_R16UI,256,frame.height,GL_RED_INTEGER,GL_UNSIGNED_SHORT,s.palette.data());
            s.plane_w=frame.width;s.plane_h=frame.height;
        } else {
            s.upload(0,frame.width,frame.height,GL_RGBA_INTEGER,GL_INT,s.top.data());
            s.upload(1,frame.width,frame.height,GL_RGBA_INTEGER,GL_INT,s.second.data());
            s.upload(2,8,frame.height,GL_RGBA_INTEGER,GL_INT,s.meta.data());
            s.upload(3,256,frame.height,GL_RED_INTEGER,GL_UNSIGNED_SHORT,s.palette.data());
        }
        const bool graded=lut && !lut->is_passthrough() && lut->native_rgb_table();
        if(graded)s.upload(6,256,128,GL_RGB,GL_UNSIGNED_BYTE,lut->native_rgb_table()->data());
        s.gl.UseProgram(s.program);s.gl.BindVertexArray(s.vao);
        constexpr const char* names[]={"top_plane","second_plane","metadata","palette_plane","surface0","surface1","grading"};
        for(unsigned i=0;i<7;++i)s.gl.Uniform1i(s.gl.GetUniformLocation(s.program,names[i]),GLint(i));
        s.gl.Uniform2i(s.gl.GetUniformLocation(s.program,"source_size"),GLint(frame.width*frame.scale),GLint(frame.height*frame.scale));
        s.gl.Uniform2i(s.gl.GetUniformLocation(s.program,"output_size"),GLint(width),GLint(height));
        s.gl.Uniform1i(s.gl.GetUniformLocation(s.program,"scale"),GLint(frame.scale));
        s.gl.Uniform1i(s.gl.GetUniformLocation(s.program,"graded"),graded);
        s.gl.Viewport(0,0,GLsizei(width),GLsizei(height));s.gl.Enable(GL_SCISSOR_TEST);
        for(unsigned first=0;first<frame.height;) {
            unsigned last=first+1;
            static const std::shared_ptr<const Hd::IndexedSurface> empty;
            const auto input=[&](unsigned row,unsigned layer)->const std::shared_ptr<const Hd::IndexedSurface>& {
                const auto& line=frame.lines[row];
                return line.kind==Hd::LineJob::AffineLine && line.affine[layer].active ? line.surfaces[layer] : empty;
            };
            const auto& first0=input(first,0);const auto& first1=input(first,1);
            while(last<frame.height && input(last,0)==first0 && input(last,1)==first1)++last;
            const GLuint bg0=s.surface(first0),bg1=s.surface(first1);
            s.bind(4,bg0);s.bind(5,bg1);
            const unsigned begin=(first*height+frame.height-1)/frame.height;
            const unsigned end=(last*height+frame.height-1)/frame.height;
            if(end>begin){s.gl.Scissor(0,GLint(begin),GLsizei(width),GLsizei(end-begin));s.gl.DrawArrays(GL_TRIANGLES,0,3);}
            first=last;
        }
        const GLenum error=s.gl.GetError();good=error==GL_NO_ERROR;
        if(!good)std::fprintf(stderr,"hd_gpu: render GL error=0x%x; portable fallback\n",unsigned(error));
        for(auto it=s.surfaces.begin();it!=s.surfaces.end();) {
            if(it->second.last!=s.frame_serial){s.gl.DeleteTextures(1,&it->second.texture);it=s.surfaces.erase(it);}
            else ++it;
        }
    }
    if(SDL_SetRenderTarget(s.renderer,old_target)!=0)good=false;
    if(!good){s.failed=true;return nullptr;}
    ++s.frames;
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
    s.last_output=s.output;
#endif
    return s.output;
}
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
SDL_Texture* HdGpuRenderer::render_native(const gba::GbaNativeDrawFrame& frame,
                                         const runtime::ColorLut* lut) {
    constexpr unsigned width=240,height=160,stride=0x60+2048;
    if(!impl_ || impl_->failed || impl_->native_failed || frame.lines().size()!=height ||
       !frame.lines().front().memory.reset || impl_->max_texture<stride)return nullptr;
    auto& s=*impl_;
    s.native_raster.resize(size_t(stride)*height);
    for(unsigned y=0;y<height;++y) {
        const auto& line=frame.lines()[y];auto* row=s.native_raster.data()+size_t(stride)*y;
        std::memcpy(row,line.io.data(),line.io.size());
        row[0]=uint8_t(line.dispcnt);row[1]=uint8_t(line.dispcnt>>8);
        std::memcpy(row+0x60,line.oam.data(),1024);
        std::memcpy(row+0x60+1024,line.palette.data(),1024);
    }
    if(!s.output || s.requested_w!=width || s.requested_h!=height) {
        auto* replacement=SDL_CreateTexture(s.renderer,SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_TARGET,width,height);
        if(!replacement)return nullptr;
        SDL_SetTextureBlendMode(replacement,SDL_BLENDMODE_NONE);
        if(s.output)SDL_DestroyTexture(s.output);
        s.output=replacement;s.width=s.requested_w=width;s.height=s.requested_h=height;
    }
    auto* old=SDL_GetRenderTarget(s.renderer);
    if(SDL_SetRenderTarget(s.renderer,s.output)!=0)return nullptr;
    if(SDL_RenderFlush(s.renderer)!=0 || SDL_GL_GetCurrentContext()!=s.context) {
        SDL_SetRenderTarget(s.renderer,old);return nullptr;
    }
    bool good=true;
    {
        State restore(s.gl);
        if(!s.native_program) {
            GLuint vertex=s.shader(GL_VERTEX_SHADER,vertex_source);
            GLuint fragment=s.shader(GL_FRAGMENT_SHADER,gba_native_fragment_source);
            if(vertex && fragment) {
                s.native_program=s.gl.CreateProgram();s.gl.AttachShader(s.native_program,vertex);
                s.gl.AttachShader(s.native_program,fragment);
                s.gl.BindFragDataLocation(s.native_program,0,"result");s.gl.LinkProgram(s.native_program);
                GLint linked=0;s.gl.GetProgramiv(s.native_program,GL_LINK_STATUS,&linked);
                if(!linked){char log[4096]{};s.gl.GetProgramInfoLog(s.native_program,sizeof(log),nullptr,log);
                    std::fprintf(stderr,"native_gpu: link unavailable: %s\n",log);good=false;}
            } else good=false;
            if(vertex)s.gl.DeleteShader(vertex);
            if(fragment)s.gl.DeleteShader(fragment);
            if(!good)s.native_failed=true;
        }
        if(good) {
            if(!s.native_planes) {
                s.image(0,GL_R8UI,256,384,GL_RED_INTEGER,GL_UNSIGNED_BYTE,nullptr);
                s.image(1,GL_R8UI,stride,height,GL_RED_INTEGER,GL_UNSIGNED_BYTE,s.native_raster.data());
                s.native_planes=true;
                for(auto& entry:s.surfaces)s.gl.DeleteTextures(1,&entry.second.texture);
                s.surfaces.clear();
            } else s.upload(1,stride,height,GL_RED_INTEGER,GL_UNSIGNED_BYTE,s.native_raster.data());
            s.gl.UseProgram(s.native_program);s.gl.BindVertexArray(s.vao);
            s.gl.Uniform1i(s.gl.GetUniformLocation(s.native_program,"vram_plane"),0);
            s.gl.Uniform1i(s.gl.GetUniformLocation(s.native_program,"raster_plane"),1);
            s.gl.Uniform1i(s.gl.GetUniformLocation(s.native_program,"grading"),6);
            s.gl.Uniform1i(s.gl.GetUniformLocation(s.native_program,"graded"),0);
            s.gl.Viewport(0,0,width,height);s.gl.Enable(GL_SCISSOR_TEST);
            const auto row_uniform=s.gl.GetUniformLocation(s.native_program,"row_id");
            for(unsigned y=0;y<height;++y) {
                const auto& memory=frame.lines()[y].memory;s.bind(0,s.textures[0]);
                for(const auto& range:memory.ranges) {
                    unsigned offset=range.offset,left=range.size,payload=range.payload_offset;
                    while(left) {
                        const unsigned x=offset&255u;
                        const unsigned bytes=x?std::min(left,256u-x):(left>=256?left/256*256:left);
                        const unsigned w=bytes>=256?256:bytes,h=bytes>=256?bytes/256:1;
                        s.gl.TexSubImage2D(GL_TEXTURE_2D,0,x,offset/256,w,h,GL_RED_INTEGER,
                            GL_UNSIGNED_BYTE,memory.payload.data()+payload);
                        offset+=bytes;payload+=bytes;left-=bytes;s.native_uploaded_bytes+=bytes;
                    }
                }
                s.gl.Uniform1i(row_uniform,y);s.gl.Scissor(0,y,width,1);s.gl.DrawArrays(GL_TRIANGLES,0,3);
            }
            if(const auto error=s.gl.GetError();error!=GL_NO_ERROR) {
                std::fprintf(stderr,"native_gpu: render GL error=0x%x; portable fallback\n",unsigned(error));good=false;
            }
        }
    }
    if(SDL_SetRenderTarget(s.renderer,old)!=0)good=false;
    if(!good){s.native_failed=true;return nullptr;}
    ++s.native_frames;s.native_output=true;s.last_output=s.output;
    return native_display(lut);
}
SDL_Texture* HdGpuRenderer::native_display(const runtime::ColorLut* lut) {
    if(!impl_ || impl_->failed || impl_->native_failed || !impl_->native_output)return nullptr;
    auto& s=*impl_;
    if(!lut || lut->is_passthrough() || !lut->native_rgb_table()) {
        s.last_output=s.output;return s.output;
    }
    if(!s.native_graded) {
        s.native_graded=SDL_CreateTexture(s.renderer,SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_TARGET,240,160);
        if(!s.native_graded)return nullptr;
        SDL_SetTextureBlendMode(s.native_graded,SDL_BLENDMODE_NONE);
    }
    auto* old=SDL_GetRenderTarget(s.renderer);
    if(SDL_SetRenderTarget(s.renderer,s.native_graded)!=0)return nullptr;
    bool good=SDL_RenderFlush(s.renderer)==0 && SDL_GL_GetCurrentContext()==s.context;
    if(good) {
        State restore(s.gl);
        if(!s.native_grading_program) {
            const auto vertex=s.shader(GL_VERTEX_SHADER,vertex_source);
            const auto fragment=s.shader(GL_FRAGMENT_SHADER,native_grading_source);
            if(vertex && fragment) {
                s.native_grading_program=s.gl.CreateProgram();
                s.gl.AttachShader(s.native_grading_program,vertex);s.gl.AttachShader(s.native_grading_program,fragment);
                s.gl.LinkProgram(s.native_grading_program);
                GLint linked=0;s.gl.GetProgramiv(s.native_grading_program,GL_LINK_STATUS,&linked);
                good=linked!=0;
            } else good=false;
            if(vertex)s.gl.DeleteShader(vertex);
            if(fragment)s.gl.DeleteShader(fragment);
        }
        s.gl.ActiveTexture(GL_TEXTURE0);
        const bool bound=SDL_GL_BindTexture(s.output,nullptr,nullptr)==0;
        good=good && bound;
        if(good) {
            s.upload(6,256,128,GL_RGB,GL_UNSIGNED_BYTE,lut->native_rgb_table()->data());
            s.gl.UseProgram(s.native_grading_program);s.gl.BindVertexArray(s.vao);
            s.gl.Uniform1i(s.gl.GetUniformLocation(s.native_grading_program,"source_plane"),0);
            s.gl.Uniform1i(s.gl.GetUniformLocation(s.native_grading_program,"grading"),6);
            s.gl.Viewport(0,0,240,160);s.gl.Enable(GL_SCISSOR_TEST);s.gl.Scissor(0,0,240,160);
            s.gl.DrawArrays(GL_TRIANGLES,0,3);good=s.gl.GetError()==GL_NO_ERROR;
        }
        if(bound){s.gl.ActiveTexture(GL_TEXTURE0);SDL_GL_UnbindTexture(s.output);}
    }
    if(SDL_SetRenderTarget(s.renderer,old)!=0)good=false;
    if(!good){s.native_failed=true;return nullptr;}
    s.last_output=s.native_graded;return s.native_graded;
}
bool HdGpuRenderer::readback_native(std::vector<uint8_t>& rgb) {
    if(!impl_ || !impl_->native_output)return false;
    auto& s=*impl_;
    auto* last=s.last_output;s.last_output=s.output;
    unsigned w=0,h=0;const bool good=readback(rgb,w,h) && w==240 && h==160;
    s.last_output=last;return good;
}
#endif
bool HdGpuRenderer::readback(std::vector<uint8_t>& rgb,unsigned& width,unsigned& height) {
    if(!impl_ || impl_->failed || !impl_->output)return false;
    auto& s=*impl_;SDL_Texture* old=SDL_GetRenderTarget(s.renderer);
    SDL_Texture* target=s.output;
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
    if(s.last_output)target=s.last_output;
#endif
    if(SDL_SetRenderTarget(s.renderer,target)!=0)return false;
    rgb.resize(size_t(s.width)*s.height*3);
    bool good=SDL_RenderReadPixels(s.renderer,nullptr,SDL_PIXELFORMAT_RGB24,rgb.data(),int(s.width*3))==0;
    if(SDL_SetRenderTarget(s.renderer,old)!=0)good=false;
    if(good){width=s.width;height=s.height;}else rgb.clear();
    return good;
}
unsigned HdGpuRenderer::width()const{return impl_?impl_->width:0;}
unsigned HdGpuRenderer::height()const{return impl_?impl_->height:0;}
void HdGpuRenderer::reset(bool context_lost){
    if(impl_){
        impl_->lost=context_lost;
        if(!context_lost && impl_->output && SDL_GL_BindTexture(impl_->output,nullptr,nullptr)==0)
            SDL_GL_UnbindTexture(impl_->output);
    }
    impl_.reset();
}
} // namespace gbarecomp
#else
namespace gbarecomp {
struct HdGpuRenderer::Impl {};
HdGpuRenderer::HdGpuRenderer()=default;HdGpuRenderer::~HdGpuRenderer()=default;
bool HdGpuRenderer::initialize(SDL_Renderer*,SDL_Texture*){return false;}
bool HdGpuRenderer::available()const{return false;}
SDL_Texture* HdGpuRenderer::render(const gba::GbaHighResolutionFrame::DrawFrame&,const runtime::ColorLut*){return nullptr;}
#if defined(GBARECOMP_NATIVE_DRAW_JOBS)
SDL_Texture* HdGpuRenderer::render_native(const gba::GbaNativeDrawFrame&,const runtime::ColorLut*){return nullptr;}
SDL_Texture* HdGpuRenderer::native_display(const runtime::ColorLut*){return nullptr;}
bool HdGpuRenderer::readback_native(std::vector<uint8_t>&){return false;}
#endif
bool HdGpuRenderer::readback(std::vector<uint8_t>&,unsigned&,unsigned&){return false;}
unsigned HdGpuRenderer::width()const{return 0;}unsigned HdGpuRenderer::height()const{return 0;}
void HdGpuRenderer::reset(bool){impl_.reset();}
}
#endif
