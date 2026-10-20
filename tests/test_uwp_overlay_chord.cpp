#include "../xbox/graphics/overlay_chord.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool value) {if(!value) std::abort();}
int main() {
    Gow3Uwp::OverlayChord chord;
    Check(!chord.Update(false,false,0,0).consume);
    Check(!chord.Update(true,false,0,0).consume);
    Check(chord.Update(true,true,0,0).consume);
    Check(!chord.Update(false,false,0,0).toggle);
    Check(!chord.Update(false,false,0,0).consume);
    Check(!chord.Update(false,false,1,1).consume);
    Check(!chord.Update(true,true,1,1).toggle);
    Check(!chord.Update(false,true,1,1).toggle);
    Check(!chord.Update(false,false,1,1).toggle);
    Check(!chord.Update(false,false,0,1).toggle);
    Check(chord.Update(false,false,0,0).toggle);
    Check(!chord.Update(false,false,0,0).toggle);
    Check(chord.Update(true,true,0,0).consume);
    Check(!chord.Update(true,true,1,1).toggle);
    Check(chord.Update(true,true,0,0).consume);
    Check(chord.Update(false,false,0,0).toggle);
    Check(chord.Update(true,true,1,0).consume);
    Check(!chord.Update(false,false,0,0).toggle);
    std::puts("PASS: overlay chord release order, input capture and one toggle per gesture");
}
