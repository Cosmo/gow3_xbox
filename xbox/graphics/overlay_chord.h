#pragma once

namespace Gow3Uwp {
struct OverlayChord {
    bool active=false,triggers=false;
    struct Result {bool consume,toggle;};
    Result Update(bool view,bool menu,double left,double right) {
        if(view&&menu) active=true;
        if(!active) return {false,false};
        triggers=triggers||(left>0.5&&right>0.5);
        const bool released=!view&&!menu&&(!triggers||(left<0.1&&right<0.1));
        const bool toggle=released&&triggers;
        if(released) active=triggers=false;
        return {true,toggle};
    }
};
}
