#include "hq2x_filter.h"
#include "../../third_party/hqx/hqx.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

static void check(bool ok,const char* text) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1); } }
int main() {
    using namespace gbarecomp;
    check(!hq2x_enabled() && hqx_scale()==0,"filter defaults off");
    Hq2xFilter filter;
    check(!filter.apply(nullptr,1,1) && !filter.apply(nullptr,0,0),"invalid input is rejected");
    std::array<std::uint8_t,3> red{255,0,0};
    check(!filter.apply(red.data(),1,1,1) && !filter.apply(red.data(),1,1,5),"unsupported factors rejected");
    check(!filter.apply(red.data(),std::numeric_limits<unsigned>::max(),1,4),"oversized row rejected before reading input");
    const auto* solid=filter.apply(red.data(),1,1);
    check(solid!=nullptr,"replaceable shared library loaded");
    for (int i=0;i<4;++i) check(solid[i*3]==255 && solid[i*3+1]==0 && solid[i*3+2]==0,"channel order and solid color preserved");
    std::array<std::uint8_t,27> steps{};
    for(int y=0;y<3;++y)for(int x=0;x<3;++x)if(x<=y)
        for(int c=0;c<3;++c)steps[(y*3+x)*3+c]=255;
    const auto before=steps;
    std::array<std::uint32_t,9> source{};
    for(unsigned i=0;i<source.size();++i)
        source[i]=0xff000000u | (std::uint32_t(steps[i*3])<<16) |
                  (std::uint32_t(steps[i*3+1])<<8) | steps[i*3+2];
    hqxInit();
    for(unsigned factor : {2u,3u,4u}) {
        std::vector<std::uint32_t> reference(9*factor*factor);
        if(factor==2) hq2x_32(source.data(),reference.data(),3,3);
        if(factor==3) hq3x_32(source.data(),reference.data(),3,3);
        if(factor==4) hq4x_32(source.data(),reference.data(),3,3);
        const auto* result=filter.apply(steps.data(),3,3,factor);
        check(result!=nullptr && steps==before,"source frame untouched");
        int intermediate=0;
        for(unsigned i=0;i<reference.size();++i) {
            check(result[i*3]==std::uint8_t(reference[i]>>16) &&
                  result[i*3+1]==std::uint8_t(reference[i]>>8) &&
                  result[i*3+2]==std::uint8_t(reference[i]),"RGB adapter matches genuine upstream HQx output");
            check(result[i*3]==result[i*3+1] && result[i*3+1]==result[i*3+2],"gray input remains gray");
            intermediate+=result[i*3]!=0 && result[i*3]!=255;
        }
        check(intermediate>0,"HQx reconstructs diagonal edges rather than nearest scaling");
        check(set_hqx_scale(factor) && hqx_scale()==factor,"only selected factor is active");
        check(!set_hqx_scale(1) && hqx_scale()==factor,"invalid selection preserves current filter");
        std::printf("HQ%ux adapter matches upstream (%d reconstructed edge pixels)\n",factor,intermediate);
    }
    set_hq2x_enabled(true);check(hq2x_enabled() && hqx_scale()==2,"legacy enable still selects HQ2x");
    set_hq2x_enabled(false);check(!hq2x_enabled() && hqx_scale()==0,"legacy reset disables every filter");
    std::printf("HQx shared filter: passed\n");
}
