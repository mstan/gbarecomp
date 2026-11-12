#include "hq2x_filter.h"
#include <array>
#include <cstdio>
#include <cstdlib>

static void check(bool ok,const char* text) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1); } }
int main() {
    using namespace gbarecomp;
    check(!hq2x_enabled(),"filter defaults off");
    Hq2xFilter filter;
    check(!filter.apply(nullptr,1,1) && !filter.apply(nullptr,0,0),"invalid input is rejected");
    std::array<std::uint8_t,3> red{255,0,0};
    const auto* solid=filter.apply(red.data(),1,1);
    check(solid!=nullptr,"replaceable shared library loaded");
    for (int i=0;i<4;++i) check(solid[i*3]==255 && solid[i*3+1]==0 && solid[i*3+2]==0,"channel order and solid color preserved");
    std::array<std::uint8_t,27> steps{};
    for(int y=0;y<3;++y)for(int x=0;x<3;++x)if(x<=y)
        for(int c=0;c<3;++c)steps[(y*3+x)*3+c]=255;
    const auto before=steps;
    const auto* result=filter.apply(steps.data(),3,3);
    check(result!=nullptr && steps==before,"source frame untouched");
    int intermediate=0;
    for(int i=0;i<36;++i) {
        check(result[i*3]==result[i*3+1] && result[i*3+1]==result[i*3+2],"gray input remains gray");
        intermediate+=result[i*3]!=0 && result[i*3]!=255;
    }
    check(intermediate>0,"HQ2x reconstructs diagonal edges rather than nearest scaling");
    set_hq2x_enabled(true);check(hq2x_enabled(),"enable selection");
    set_hq2x_enabled(false);check(!hq2x_enabled(),"reset selection");
    std::printf("HQ2x shared filter: passed (%d reconstructed edge pixels)\n",intermediate);
}
