#include "client/camera.h"
#include "client/touch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 1000.0f
#define HEIGHT 2000.0f
#define SLOP 10.0f

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "touch test failed: %s\n", message);
        exit(1);
    }
}

static SosControls Take(SosTouch *touch, double seconds)
{
    return SosTouchTakeControls(touch, seconds, WIDTH, HEIGHT);
}

static int Idle(const SosControls *controls)
{
    return !controls->rotating && controls->zoomSteps == 0.0f && controls->dragForward == 0.0f &&
           controls->dragRight == 0.0f && !controls->followNext && !controls->togglePause &&
           controls->speedPreset == -1;
}

static void TestTurn(void)
{
    SosTouch touch;
    SosTouchInit(&touch, SLOP);
    SosTouchEvent(&touch, SOS_TOUCH_DOWN, 7, 100.0f, 500.0f, 0.0);
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 7, 105.0f, 500.0f, 0.02);
    SosControls controls = Take(&touch, 0.02);
    Expect(Idle(&controls), "movement within the slop does nothing");
    /* Across the whole width: half a turn, in whole steps over two frames. */
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 7, 105.0f + WIDTH / 2.0f, 500.0f, 0.04);
    const SosControls first = Take(&touch, 0.04);
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 7, 105.0f + WIDTH, 500.0f, 0.06);
    const SosControls second = Take(&touch, 0.06);
    const double turned = (double)(first.rotateX + second.rotateX) * SOS_CAMERA_RADIANS_PER_STEP;
    Expect(first.rotating && second.rotating && fabs(turned - 3.14159) < 0.01,
           "a drag across the width turns half around");
    Expect(first.rotateY == 0 && second.rotateY == 0, "a level drag does not tilt");
    SosTouchEvent(&touch, SOS_TOUCH_UP, 7, 105.0f + WIDTH, 500.0f, 0.08);
    controls = Take(&touch, 0.5);
    Expect(Idle(&controls), "a drag is not a tap");
}

static void TestPinchAndDrag(void)
{
    SosTouch touch;
    SosTouchInit(&touch, SLOP);
    SosTouchEvent(&touch, SOS_TOUCH_DOWN, 1, 400.0f, 1000.0f, 0.0);
    SosTouchEvent(&touch, SOS_TOUCH_DOWN, 2, 600.0f, 1000.0f, 0.01);
    /* Spread from 200 to 400 pixels apart around the same middle. */
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 1, 300.0f, 1000.0f, 0.02);
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 2, 700.0f, 1000.0f, 0.02);
    SosControls controls = Take(&touch, 0.02);
    const double expected = log(2.0) / -log(SOS_CAMERA_ZOOM_FACTOR);
    Expect(fabs((double)controls.zoomSteps - expected) < 0.01, "spreading twice apart zooms in");
    Expect(fabs((double)controls.dragForward) < 1e-4 && fabs((double)controls.dragRight) < 1e-4,
           "a symmetric pinch does not drag");
    Expect(!controls.rotating, "two fingers do not turn");

    /* Both fingers down a tenth of the height: the ground follows them. */
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 1, 300.0f, 1200.0f, 0.04);
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 2, 700.0f, 1200.0f, 0.04);
    controls = Take(&touch, 0.04);
    const double distances = 0.1 * 2.0 * tan(0.5 * (double)SOS_CAMERA_FIELD_OF_VIEW);
    Expect(fabs((double)controls.dragForward - distances) < 1e-4 &&
               fabs((double)controls.dragRight) < 1e-4,
           "dragging down pulls the ground towards the viewer");
    Expect(fabs((double)controls.zoomSteps) < 1e-4, "a parallel drag does not zoom");

    /* The finger left behind does not turn the camera. */
    SosTouchEvent(&touch, SOS_TOUCH_UP, 2, 700.0f, 1200.0f, 0.06);
    SosTouchEvent(&touch, SOS_TOUCH_MOVE, 1, 600.0f, 1500.0f, 0.08);
    controls = Take(&touch, 0.08);
    Expect(!controls.rotating, "the remaining finger does not turn");
    SosTouchEvent(&touch, SOS_TOUCH_UP, 1, 600.0f, 1500.0f, 0.1);
    controls = Take(&touch, 0.5);
    Expect(Idle(&controls), "a pinch is not a tap");
}

static void Tap(SosTouch *touch, int32_t id, float x, float y, double seconds)
{
    SosTouchEvent(touch, SOS_TOUCH_DOWN, id, x, y, seconds);
    SosTouchEvent(touch, SOS_TOUCH_UP, id, x + 2.0f, y, seconds + 0.08);
}

static void TestTaps(void)
{
    SosTouch touch;
    SosTouchInit(&touch, SLOP);
    Tap(&touch, 1, 500.0f, 500.0f, 0.0);
    SosControls controls = Take(&touch, 0.1);
    Expect(!controls.followNext, "one tap does not follow");
    Tap(&touch, 2, 505.0f, 498.0f, 0.25);
    controls = Take(&touch, 0.35);
    Expect(controls.followNext, "a double tap follows the next person");
    controls = Take(&touch, 0.4);
    Expect(!controls.followNext, "a double tap is reported once");

    Tap(&touch, 3, 500.0f, 500.0f, 1.0);
    Tap(&touch, 4, 500.0f, 500.0f, 1.6);
    controls = Take(&touch, 1.7);
    Expect(!controls.followNext, "slow taps are not a double tap");
    Tap(&touch, 5, 900.0f, 1500.0f, 1.8);
    controls = Take(&touch, 1.9);
    Expect(!controls.followNext, "distant taps are not a double tap");
}

static void TestPress(void)
{
    SosTouch touch;
    SosTouchInit(&touch, SLOP);
    SosTouchEvent(&touch, SOS_TOUCH_DOWN, 1, 500.0f, 500.0f, 0.0);
    SosControls controls = Take(&touch, 0.3);
    Expect(!controls.togglePause, "a short hold is no press");
    controls = Take(&touch, 0.7);
    Expect(controls.togglePause, "a long press pauses or resumes");
    controls = Take(&touch, 1.5);
    Expect(!controls.togglePause, "a press is reported once");
    SosTouchEvent(&touch, SOS_TOUCH_UP, 1, 500.0f, 500.0f, 1.6);
    Tap(&touch, 2, 500.0f, 500.0f, 1.7);
    controls = Take(&touch, 1.8);
    Expect(!controls.followNext, "a press and a tap are not a double tap");

    SosTouchEvent(&touch, SOS_TOUCH_DOWN, 3, 500.0f, 500.0f, 3.0);
    SosTouchEvent(&touch, SOS_TOUCH_CANCEL, 3, 500.0f, 500.0f, 3.1);
    controls = Take(&touch, 4.0);
    Expect(Idle(&controls), "a cancelled touch does nothing");
    Expect(touch.count == 0u, "a cancel lifts every finger");
}

int main(void)
{
    TestTurn();
    TestPinchAndDrag();
    TestTaps();
    TestPress();
    return 0;
}
