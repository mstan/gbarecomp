#pragma once

// Hardware native-resolution raster from one owned historical row. Unknown
// presentation providers are excluded by the producer, before capture. All
// integer bounds derive from16-bit registers,28-bit references and240x160.
inline constexpr const char* gba_native_fragment_source=R"GLSL(#version 130
uniform usampler2D vram_plane, raster_plane;
uniform sampler2D grading;
uniform int row_id, graded;
out vec4 result;
uint byte_at(int offset) { return texelFetch(raster_plane,ivec2(offset,row_id),0).r; }
int half_at(int offset) { return int(byte_at(offset)|(byte_at(offset+1)<<8u)); }
int signed_half(int offset) { int v=half_at(offset);return v>=32768?v-65536:v; }
int reference_at(int offset) {
    int v=half_at(offset)|((half_at(offset+2)&4095)<<16);
    return v>=134217728?v-268435456:v;
}
uint vram(int offset) {
    if(offset<0 || offset>=98304)return 0u;
    return texelFetch(vram_plane,ivec2(offset&255,offset>>8),0).r;
}
uint vhalf(int offset) { return vram(offset)|(vram(offset+1)<<8u); }
uint palette(int index) { return uint(half_at(1120+index*2)); }
int floor256(int v) { return v>=0?v/256:-((-v+255)/256); }
struct Pixel { uint color; int key,layer; bool t1,t2,valid; };
Pixel empty_pixel() { return Pixel(0u,2147483647,5,false,false,false); }
void insert_pixel(inout Pixel top,inout Pixel second,Pixel candidate) {
    if(!candidate.valid)return;
    if(candidate.key<top.key){second=top;top=candidate;}
    else if(candidate.key<second.key)second=candidate;
}
bool object_pixel(int index,int x,int y,int dispcnt,out uint color,out int mode,out int key) {
    int base=96+index*8;
    int a0=half_at(base),a1=half_at(base+2),a2=half_at(base+4);
    bool affine=(a0&256)!=0,doubled=(a0&512)!=0;
    mode=(a0>>10)&3;key=((a2>>10)&3)*256+index;
    if((!affine && doubled) || mode==3)return false;
    int shape=(a0>>14)&3,size=(a1>>14)&3;
    if(shape==3)return false;
    const ivec2 shapes[12]=ivec2[12](
        ivec2(8,8),ivec2(16,16),ivec2(32,32),ivec2(64,64),
        ivec2(16,8),ivec2(32,8),ivec2(32,16),ivec2(64,32),
        ivec2(8,16),ivec2(8,32),ivec2(16,32),ivec2(32,64));
    ivec2 dimensions=shapes[shape*4+size];
    int sx=a1&511,sy=a0&255;
    if((sx&256)!=0)sx-=512;if(sy>=160)sy-=256;
    int tile=a2&1023;
    if((dispcnt&7)>=3 && tile<512)return false;
    ivec2 point=ivec2(x-sx,y-sy);
    if(affine) {
        ivec2 bounds=dimensions*(doubled?2:1);
        if(any(lessThan(point,ivec2(0))) || any(greaterThanEqual(point,bounds)))return false;
        ivec2 delta=point-bounds/2;
        int group=96+((a1>>9)&31)*32;
        point=ivec2(floor256(signed_half(group+6)*delta.x+signed_half(group+14)*delta.y),
                    floor256(signed_half(group+22)*delta.x+signed_half(group+30)*delta.y))+dimensions/2;
    } else {
        if((a1&4096)!=0)point.x=dimensions.x-1-point.x;
        if((a1&8192)!=0)point.y=dimensions.y-1-point.y;
    }
    if(any(lessThan(point,ivec2(0))) || any(greaterThanEqual(point,dimensions)))return false;
    bool color256=(a0&8192)!=0;
    ivec2 cell=point/8,within=point&ivec2(7);
    if((dispcnt&64)!=0)tile+=(cell.y*(dimensions.x/8)+cell.x)*(color256?2:1);
    else tile+=cell.y*32+cell.x*(color256?2:1);
    int offset=65536+tile*32+within.y*(color256?8:4)+(color256?within.x:within.x/2);
    uint index_value=vram(offset);
    if(!color256)index_value=(index_value>>uint((within.x&1)*4))&15u;
    if(index_value==0u)return false;
    if(!color256)index_value|=uint((a2>>12)&15)<<4u;
    color=palette(256+int(index_value));return true;
}
bool in_window(int coordinate,int value,int limit) {
    int first=(value>>8)&255,last=value&255;
    if(last>limit || first>last)last=limit;
    return coordinate>=first && coordinate<last;
}
bool background(int layer,int x,int y,int dispcnt,out uint color,out int key) {
    int mode=dispcnt&7,cnt=half_at(8+layer*2);
    key=(cnt&3)*256+128+layer;
    if((dispcnt&(256<<layer))==0)return false;
    int character=((cnt>>2)&3)*16384,screen=((cnt>>8)&31)*2048;
    if(mode==0 || (mode==1 && layer<2)) {
        int size=(cnt>>14)&3,width=(size&1)!=0?512:256,height=(size&2)!=0?512:256;
        int tx=(x+(half_at(16+layer*4)&511))&(width-1);
        int ty=(y+(half_at(18+layer*4)&511))&(height-1);
        int cx=tx/8,cy=ty/8;
        int offset=screen+((cx/32)+(cy/32)*(width/256))*2048+((cy&31)*32+(cx&31))*2;
        if(offset+1>=98304)return false;
        int entry=int(vhalf(offset));
        int px=tx&7,py=ty&7;
        if((entry&1024)!=0)px=7-px;if((entry&2048)!=0)py=7-py;
        bool color256=(cnt&128)!=0;
        offset=character+(entry&1023)*(color256?64:32)+py*(color256?8:4)+(color256?px:px/2);
        uint index_value=vram(offset);
        if(!color256)index_value=(index_value>>uint((px&1)*4))&15u;
        if(index_value==0u)return false;
        if(!color256)index_value|=uint((entry>>12)&15)<<4u;
        color=palette(int(index_value));return true;
    }
    bool affine=(mode==1 && layer==2) || (mode==2 && layer>=2);
    bool bitmap=mode>=3 && mode<=5 && layer==2;
    if(!affine && !bitmap)return false;
    int base=32+(layer-2)*16;
    int u=reference_at(base+8)+y*signed_half(base+2)+x*signed_half(base);
    int v=reference_at(base+12)+y*signed_half(base+6)+x*signed_half(base+4);
    int tx=floor256(u),ty=floor256(v);
    if(affine) {
        int size=128<<((cnt>>14)&3);
        if((cnt&8192)!=0){tx&=size-1;ty&=size-1;}
        else if(tx<0 || ty<0 || tx>=size || ty>=size)return false;
        int offset=screen+(ty/8)*(size/8)+tx/8;
        if(offset>=98304)return false;
        offset=character+int(vram(offset))*64+(ty&7)*8+(tx&7);
        uint index_value=vram(offset);if(index_value==0u)return false;
        color=palette(int(index_value));return true;
    }
    int width=mode==5?160:240,height=mode==5?128:160;
    if(tx<0 || ty<0 || tx>=width || ty>=height)return false;
    int offset=(mode!=3 && (dispcnt&16)!=0?40960:0)+(ty*width+tx)*(mode==4?1:2);
    if(mode==4){uint index_value=vram(offset);if(index_value==0u)return false;color=palette(int(index_value));}
    else color=vhalf(offset);
    return true;
}
uint blend(uint a,uint b,int eva,int evb) {
    eva=min(eva,16);evb=min(evb,16);
    int r=min(31,(int(a&31u)*eva+int(b&31u)*evb+8)/16);
    int ag=int(((a>>4u)&62u)|(a>>15u)),bg=int(((b>>4u)&62u)|(b>>15u));
    int g=min(63,(ag*eva+bg*evb+8)/16);
    int blue=min(31,(int((a>>10u)&31u)*eva+int((b>>10u)&31u)*evb+8)/16);
    return uint(r|((g/2)<<5)|(blue<<10));
}
uint brightness(uint c,int value,bool light) {
    value=min(value,16);
    ivec3 v=ivec3(int(c&31u),int(((c>>4u)&62u)|(c>>15u)),int((c>>10u)&31u));
    if(light)v+=((ivec3(31,63,31)-v)*value+ivec3(8))/16;
    else v-=(v*value+ivec3(7))/16;
    return uint(v.x|((v.y/2)<<5)|(v.z<<10));
}
void output_color(uint color) {
    ivec3 c=ivec3(int(color&31u),int((color>>5u)&31u),int((color>>10u)&31u));
    ivec3 rgb=(c<<3)|(c>>2);
    if(graded!=0){int index=(rgb.x>>3)|((rgb.y>>3)<<5)|((rgb.z>>3)<<10);result=vec4(texelFetch(grading,ivec2(index&255,index>>8),0).rgb,1.0);}
    else result=vec4(vec3(rgb)/255.0,1.0);
}
void main() {
    int x=int(gl_FragCoord.x),y=row_id,dispcnt=half_at(0);
    if((dispcnt&128)!=0){output_color(32767u);return;}
    int bldcnt=half_at(80),alpha=half_at(82),effect=(bldcnt>>6)&3;
    Pixel object_top=empty_pixel(),object_second=empty_pixel();bool object_window=false;
    if((dispcnt&36864)!=0)for(int index=0;index<128;++index) {
        uint color;int mode,key;
        if(!object_pixel(index,x,y,dispcnt,color,mode,key))continue;
        if(mode==2){if((dispcnt&32768)!=0)object_window=true;continue;}
        if((dispcnt&4096)!=0)insert_pixel(object_top,object_second,Pixel(color,key,4,mode==1,(bldcnt&4096)!=0,true));
    }
    int window=63;
    if((dispcnt&57344)!=0) {
        if((dispcnt&8192)!=0 && in_window(y,half_at(68),160) && in_window(x,half_at(64),240))window=half_at(72)&63;
        else if((dispcnt&16384)!=0 && in_window(y,half_at(70),160) && in_window(x,half_at(66),240))window=(half_at(72)>>8)&63;
        else window=(half_at(74)>>(object_window?8:0))&63;
    }
    Pixel top=Pixel(palette(0),1879048192,5,(window&32)!=0 && (bldcnt&32)!=0,(bldcnt&8192)!=0,true),second=empty_pixel();
    if((window&16)!=0) {
        object_top.t1=object_top.t1 && (window&32)!=0;object_second.t1=object_second.t1 && (window&32)!=0;
        insert_pixel(top,second,object_top);insert_pixel(top,second,object_second);
    }
    for(int layer=0;layer<4;++layer) {
        if((window&(1<<layer))==0)continue;
        uint color;int key;
        if(background(layer,x,y,dispcnt,color,key))insert_pixel(top,second,Pixel(color,key,layer,
            (window&32)!=0 && (bldcnt&(1<<layer))!=0,(bldcnt&(256<<layer))!=0,true));
    }
    uint color=top.color;
    if((effect==1 || top.layer==4) && top.t1 && second.valid && second.t2 && !(top.layer==4 && second.layer==4))color=blend(color,second.color,alpha&31,(alpha>>8)&31);
    else if(top.t1 && effect==2)color=brightness(color,half_at(84)&31,true);
    else if(top.t1 && effect==3)color=brightness(color,half_at(84)&31,false);
    output_color(color);
}
)GLSL";
