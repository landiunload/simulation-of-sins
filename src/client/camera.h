#ifndef SOS_CLIENT_CAMERA_H
#define SOS_CLIENT_CAMERA_H

#include "scene/math_service.h"
#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* The god's view: an orbit around a point on the world, from kilometres
 * above a landscape down to a flower. The focus is an integer world position,
 * so the camera works the same anywhere in the unbounded world; floats appear
 * only relative to the render origin near the focus. */

#define SOS_CAMERA_MIN_DISTANCE 2.0
#define SOS_CAMERA_MAX_DISTANCE 80000.0

typedef struct SosCamera
{
    SosPosition focus;
    /* Metres from the focus to the eye. */
    double distance;
    /* Radians: heading around the vertical, and elevation above the
     * horizon. */
    double yaw;
    double pitch;
} SosCamera;

typedef struct SosCameraView
{
    /* Integer origin near the focus; draw origins are relative to it. */
    SosPosition renderOrigin;
    float viewProjection[16];
    /* The eye relative to the render origin, in metres. */
    float eye[3];
    float nearPlane;
    float farPlane;
} SosCameraView;

void SosCameraInit(SosCamera *camera, SosPosition focus);
/* Positive steps move closer; each step changes the distance by 15 %. */
void SosCameraZoom(SosCamera *camera, float steps);
void SosCameraRotate(SosCamera *camera, int32_t deltaX, int32_t deltaY);
/* Moves the focus along the ground; speed grows with the distance, so a
 * second covers about the visible area at any zoom. */
void SosCameraPan(SosCamera *camera, float forward, float right, double seconds);
bool SosCameraBuildView(const SosCamera *camera, const LaiueSceneMathServiceV1 *math,
                        float aspectRatio, SosCameraView *outView);

#endif
