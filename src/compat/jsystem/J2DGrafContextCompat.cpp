// compat/jsystem — host completions of the J2D pieces that the vendored
// JSystem sources leave undefined: the JGeometry::TBox2<f32>::operator=
// body (declared-only in TBox.hpp) and J2DOrthoGraph::getGrafType
// (declared as an override but missing from J2DOrthoGraph.cpp).
#include <JSystem/J2DGraph/J2DGrafContext.hpp>
#include <JSystem/J2DGraph/J2DOrthoGraph.hpp>

// JGeometry::TBox2<f32>::operator= — the vendored TBox.hpp only declares it
// (the body lives in the not-yet-compiled JGeometry.cpp). The vendored
// J2DOrthoGraph/J2DGrafContext code assigns TBox2f values, so provide the
// body as an explicit specialization for the instantiation they use. It must
// come before any use of the assignment in this TU (after-instantiation
// error). NOTE: when the real JGeometry.cpp is compiled, drop this block.
template <>
void JGeometry::TBox2< f32 >::operator=(const JGeometry::TBox2< f32 >& other) {
    i = other.i;
    f = other.f;
}


// J2DGrafContext::place(f32,f32,f32,f32) — the vendored J2DGrafContext.cpp
// defines the TBox2f overload but not this one (the decomp omits it).
void J2DGrafContext::place(f32 x, f32 y, f32 width, f32 height) {
    place(JGeometry::TBox2< f32 >(x, y, x + width, y + height));
}

// J2DOrthoGraph::getGrafType — declared as an override in the header but not
// implemented by the vendored J2DOrthoGraph.cpp; the documented type for the
// ortho graph is J2DGraf_Ortho.
J2DGrafType J2DOrthoGraph::getGrafType() const {
    return J2DGraf_Ortho;
}
