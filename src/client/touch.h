#ifndef SOS_CLIENT_TOUCH_H
#define SOS_CLIENT_TOUCH_H

#include "client/scene.h"

#include <stdbool.h>
#include <stdint.h>

/* Touch gestures to player intent, independent of any platform's touch API:
 *
 * - one finger drags to turn the camera;
 * - two fingers pinch to zoom and drag the ground;
 * - a double tap follows the next person;
 * - a long press pauses or resumes time.
 *
 * Positions are pixels with Y pointing down; times are seconds of any
 * monotonic clock. Fingers beyond the second are ignored. */

#define SOS_TOUCH_MAX_POINTERS 2u

typedef enum SosTouchPhase
{
    SOS_TOUCH_DOWN = 0,
    SOS_TOUCH_MOVE,
    SOS_TOUCH_UP,
    /* The platform took the gesture away: no tap or press comes of it. */
    SOS_TOUCH_CANCEL,
} SosTouchPhase;

typedef struct SosTouchPointer
{
    int32_t id;
    float x;
    float y;
} SosTouchPointer;

typedef struct SosTouch
{
    /* Movement below this many pixels still counts as a tap or a press. */
    float slop;
    SosTouchPointer pointers[SOS_TOUCH_MAX_POINTERS];
    uint32_t count;
    /* The gesture since the first finger went down. */
    double startSeconds;
    float startX;
    float startY;
    bool moved;
    bool twoFingers;
    bool pressed;
    /* The previous tap, waiting for a second one. */
    bool tapWaiting;
    double tapSeconds;
    float tapX;
    float tapY;
    /* Intent gathered since the last SosTouchTakeControls. */
    float turnX;
    float turnY;
    float zoomSteps;
    float dragX;
    float dragY;
    bool followNext;
    bool togglePause;
} SosTouch;

/* `slopPixels` is about a millimetre and a half of the screen. */
void SosTouchInit(SosTouch *touch, float slopPixels);
void SosTouchEvent(SosTouch *touch, SosTouchPhase phase, int32_t id, float x, float y,
                   double seconds);
/* The intent gathered since the last call, for a view of this size in
 * pixels. Call once per frame: a long press is recognised over time. */
SosControls SosTouchTakeControls(SosTouch *touch, double seconds, float viewWidth,
                                 float viewHeight);

#endif
