#include "client/touch.h"

#include "client/camera.h"

#include <math.h>
#include <stddef.h>

/* A tap is short; a second one soon after and near the first makes a double
 * tap; a finger held still this long is a press. */
#define TAP_SECONDS 0.3
#define DOUBLE_TAP_SECONDS 0.35
#define DOUBLE_TAP_REACH 4.0f
#define PRESS_SECONDS 0.6
#define PI 3.14159265358979323846

void SosTouchInit(SosTouch *touch, float slopPixels)
{
    if (touch != NULL)
    {
        *touch = (SosTouch){.slop = slopPixels > 1.0f ? slopPixels : 1.0f};
    }
}

static float Distance(float x0, float y0, float x1, float y1)
{
    return hypotf(x1 - x0, y1 - y0);
}

static int32_t Find(const SosTouch *touch, int32_t id)
{
    for (uint32_t index = 0u; index < touch->count; ++index)
    {
        if (touch->pointers[index].id == id)
        {
            return (int32_t)index;
        }
    }
    return -1;
}

static void Down(SosTouch *touch, int32_t id, float x, float y, double seconds)
{
    if (Find(touch, id) >= 0 || touch->count == SOS_TOUCH_MAX_POINTERS)
    {
        return;
    }
    if (touch->count == 0u)
    {
        touch->startSeconds = seconds;
        touch->startX = x;
        touch->startY = y;
        touch->moved = false;
        touch->twoFingers = false;
        touch->pressed = false;
    }
    touch->pointers[touch->count++] = (SosTouchPointer){.id = id, .x = x, .y = y};
    if (touch->count == 2u)
    {
        touch->twoFingers = true;
    }
}

static void Move(SosTouch *touch, int32_t id, float x, float y)
{
    const int32_t index = Find(touch, id);
    if (index < 0)
    {
        return;
    }
    SosTouchPointer *pointer = &touch->pointers[index];
    if (touch->count == 2u)
    {
        const SosTouchPointer *other = &touch->pointers[1 - index];
        const float before = Distance(pointer->x, pointer->y, other->x, other->y);
        const float after = Distance(x, y, other->x, other->y);
        if (before >= 1.0f && after >= 1.0f)
        {
            /* Spreading the fingers by a zoom factor is one step closer. */
            touch->zoomSteps +=
                (float)(log((double)after / (double)before) / -log(SOS_CAMERA_ZOOM_FACTOR));
        }
        /* The middle between the fingers moves half as far as one of them. */
        touch->dragX += 0.5f * (x - pointer->x);
        touch->dragY += 0.5f * (y - pointer->y);
    }
    else if (!touch->twoFingers)
    {
        /* Turning starts beyond the slop, so a tap never nudges the camera. */
        if (!touch->moved && Distance(touch->startX, touch->startY, x, y) > touch->slop)
        {
            touch->moved = true;
        }
        if (touch->moved)
        {
            touch->turnX += x - pointer->x;
            touch->turnY += y - pointer->y;
        }
    }
    pointer->x = x;
    pointer->y = y;
}

static void Up(SosTouch *touch, int32_t id, float x, float y, double seconds)
{
    const int32_t index = Find(touch, id);
    if (index < 0)
    {
        return;
    }
    Move(touch, id, x, y);
    const uint32_t last = touch->count - 1u;
    if (last >= SOS_TOUCH_MAX_POINTERS)
    {
        /* Not a state this file makes; start over rather than read past. */
        touch->count = 0u;
        return;
    }
    touch->pointers[index] = touch->pointers[last];
    touch->count = last;
    if (touch->count != 0u)
    {
        /* The finger left behind after two does not turn the camera. */
        return;
    }
    const bool tap = !touch->moved && !touch->twoFingers && !touch->pressed &&
                     seconds - touch->startSeconds <= TAP_SECONDS;
    if (!tap)
    {
        touch->tapWaiting = false;
    }
    else if (touch->tapWaiting && seconds - touch->tapSeconds <= DOUBLE_TAP_SECONDS &&
             Distance(touch->tapX, touch->tapY, x, y) <= DOUBLE_TAP_REACH * touch->slop)
    {
        touch->followNext = true;
        touch->tapWaiting = false;
    }
    else
    {
        touch->tapWaiting = true;
        touch->tapSeconds = seconds;
        touch->tapX = x;
        touch->tapY = y;
    }
}

void SosTouchEvent(SosTouch *touch, SosTouchPhase phase, int32_t id, float x, float y,
                   double seconds)
{
    if (touch == NULL)
    {
        return;
    }
    switch (phase)
    {
    case SOS_TOUCH_DOWN:
        Down(touch, id, x, y, seconds);
        break;
    case SOS_TOUCH_MOVE:
        Move(touch, id, x, y);
        break;
    case SOS_TOUCH_UP:
        Up(touch, id, x, y, seconds);
        break;
    case SOS_TOUCH_CANCEL:
    default:
        touch->count = 0u;
        touch->tapWaiting = false;
        break;
    }
}

SosControls SosTouchTakeControls(SosTouch *touch, double seconds, float viewWidth, float viewHeight)
{
    SosControls controls = {.speedPreset = -1};
    if (touch == NULL || !(viewWidth > 0.0f) || !(viewHeight > 0.0f))
    {
        return controls;
    }
    if (touch->count == 1u && !touch->moved && !touch->twoFingers && !touch->pressed &&
        seconds - touch->startSeconds >= PRESS_SECONDS)
    {
        touch->pressed = true;
        touch->togglePause = true;
        touch->tapWaiting = false;
    }

    /* A drag across the whole width turns the camera half around. Whole
     * steps go out; the rest waits for the next frame. */
    const float stepsPerPixel = (float)(PI / SOS_CAMERA_RADIANS_PER_STEP) / viewWidth;
    controls.rotateX = (int32_t)(touch->turnX * stepsPerPixel);
    controls.rotateY = (int32_t)(touch->turnY * stepsPerPixel);
    controls.rotating = controls.rotateX != 0 || controls.rotateY != 0;
    touch->turnX -= (float)controls.rotateX / stepsPerPixel;
    touch->turnY -= (float)controls.rotateY / stepsPerPixel;

    /* At the focus the view is this many camera distances high, so the
     * ground follows the fingers. */
    const float distancesPerPixel = 2.0f * tanf(0.5f * SOS_CAMERA_FIELD_OF_VIEW) / viewHeight;
    controls.zoomSteps = touch->zoomSteps;
    controls.dragForward = touch->dragY * distancesPerPixel;
    controls.dragRight = -touch->dragX * distancesPerPixel;
    controls.followNext = touch->followNext;
    controls.togglePause = touch->togglePause;
    touch->zoomSteps = 0.0f;
    touch->dragX = 0.0f;
    touch->dragY = 0.0f;
    touch->followNext = false;
    touch->togglePause = false;
    return controls;
}
