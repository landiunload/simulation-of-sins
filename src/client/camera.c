#include "client/camera.h"

#include <math.h>
#include <stddef.h>

#define SOS_CAMERA_MIN_PITCH 0.15
#define SOS_CAMERA_MAX_PITCH 1.55
/* The render origin snaps to this grid, so it changes rarely and stays within
 * half a kilometre of the focus. */
#define SOS_CAMERA_ORIGIN_SHIFT 20u

void SosCameraInit(SosCamera *camera, SosPosition focus)
{
    *camera = (SosCamera){
        .focus = focus,
        .distance = 60.0,
        .yaw = 0.6,
        .pitch = 0.9,
    };
}

void SosCameraZoom(SosCamera *camera, float steps)
{
    camera->distance *= pow(SOS_CAMERA_ZOOM_FACTOR, (double)steps);
    if (camera->distance < SOS_CAMERA_MIN_DISTANCE)
    {
        camera->distance = SOS_CAMERA_MIN_DISTANCE;
    }
    if (camera->distance > SOS_CAMERA_MAX_DISTANCE)
    {
        camera->distance = SOS_CAMERA_MAX_DISTANCE;
    }
}

void SosCameraRotate(SosCamera *camera, int32_t deltaX, int32_t deltaY)
{
    camera->yaw += (double)deltaX * SOS_CAMERA_RADIANS_PER_STEP;
    camera->pitch += (double)deltaY * SOS_CAMERA_RADIANS_PER_STEP;
    camera->yaw = fmod(camera->yaw, 6.283185307179586);
    if (camera->pitch < SOS_CAMERA_MIN_PITCH)
    {
        camera->pitch = SOS_CAMERA_MIN_PITCH;
    }
    if (camera->pitch > SOS_CAMERA_MAX_PITCH)
    {
        camera->pitch = SOS_CAMERA_MAX_PITCH;
    }
}

void SosCameraPan(SosCamera *camera, float forward, float right, double seconds)
{
    const double metres = camera->distance * seconds;
    /* Forward on the ground is the view direction without its vertical part. */
    const double forwardX = sin(camera->yaw);
    const double forwardY = cos(camera->yaw);
    const double dx = (forwardX * (double)forward + forwardY * (double)right) * metres;
    const double dy = (forwardY * (double)forward - forwardX * (double)right) * metres;
    camera->focus.x += (int64_t)llround(dx * SOS_UNITS_PER_METRE);
    camera->focus.y += (int64_t)llround(dy * SOS_UNITS_PER_METRE);
}

bool SosCameraBuildView(const SosCamera *camera, const LaiueSceneMathServiceV1 *math,
                        float aspectRatio, SosCameraView *outView)
{
    if (camera == NULL || math == NULL || outView == NULL || !(aspectRatio > 0.0f))
    {
        return false;
    }
    const int64_t mask = ~((INT64_C(1) << SOS_CAMERA_ORIGIN_SHIFT) - 1);
    outView->renderOrigin = (SosPosition){
        .x = camera->focus.x & mask,
        .y = camera->focus.y & mask,
        .z = camera->focus.z & mask,
    };
    const float focus[3] = {
        (float)((double)(camera->focus.x - outView->renderOrigin.x) / SOS_UNITS_PER_METRE),
        (float)((double)(camera->focus.y - outView->renderOrigin.y) / SOS_UNITS_PER_METRE),
        (float)((double)(camera->focus.z - outView->renderOrigin.z) / SOS_UNITS_PER_METRE),
    };
    const double horizontal = cos(camera->pitch) * camera->distance;
    outView->eye[0] = focus[0] - (float)(sin(camera->yaw) * horizontal);
    outView->eye[1] = focus[1] - (float)(cos(camera->yaw) * horizontal);
    outView->eye[2] = focus[2] + (float)(sin(camera->pitch) * camera->distance);

    /* Planes follow the zoom: close up the near plane reaches a flower, far
     * out the far plane reaches the horizon of a continent. */
    outView->nearPlane = (float)fmax(0.05, camera->distance * 0.005);
    outView->farPlane = (float)fmax(4000.0, camera->distance * 40.0);

    static const float up[3] = {0.0f, 0.0f, 1.0f};
    float view[16];
    float projection[16];
    if (!math->matrix4LookAt(outView->eye, focus, up, view) ||
        !math->matrix4Perspective(SOS_CAMERA_FIELD_OF_VIEW, aspectRatio, outView->nearPlane,
                                  outView->farPlane, projection))
    {
        return false;
    }
    math->matrix4Multiply(view, projection, outView->viewProjection);
    return true;
}
