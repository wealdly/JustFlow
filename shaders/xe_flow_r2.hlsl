// xe_flow with a +-2 px search (25 candidates instead of 81), for the finest pyramid levels: by then
// the parent's predictors are within a pixel or two, and the full +-4 search was two thirds of the
// whole flow's GPU time (L0: 0.51 of 0.75 ms per direction at 2880x1800 on the B390).
#define R 2
#include "xe_flow.hlsl"
