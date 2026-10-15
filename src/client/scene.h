#ifndef SOS_CLIENT_SCENE_H
#define SOS_CLIENT_SCENE_H

#include "graphics/graphics_device_v2.h"
#include "mesh/mesher_service.h"
#include "scene/math_service.h"

#include <stdbool.h>
#include <stdint.h>

/* What the player sees and steers: the simulated world, its clock, the god's
 * camera and the views of terrain, plants and people. Every engine technology
 * it uses is optional: without camera math nothing is drawn, without the
 * mesher the terrain is missing, and the world keeps living either way. */

typedef struct SosScene SosScene;

typedef struct SosSceneDesc
{
    uint64_t seed;
    double timeScale;
    /* Zero keeps the default distance. */
    double cameraDistance;
    /* NULL starts the world without views; SosSceneAttachDevice adds them. */
    LaiueGraphicsDeviceV2 *device;
    /* NULL or too old: the scene cannot place a camera and draws nothing. */
    const LaiueSceneMathServiceV1 *math;
    uint32_t mathServiceSize;
    /* NULL or without surface meshing: no terrain. */
    const LaiueMesherServiceV1 *mesher;
    uint32_t mesherServiceSize;
} SosSceneDesc;

/* One frame of player intent, independent of keyboards and mice. */
typedef struct SosControls
{
    float zoomSteps;
    bool rotating;
    int32_t rotateX;
    int32_t rotateY;
    float moveForward;
    float moveRight;
    bool moveFast;
    /* A drag of the ground in camera distances: a touch screen moves the
     * world under the fingers. */
    float dragForward;
    float dragRight;
    bool togglePause;
    /* -1 keeps the speed; 0..5 picks real time, a minute, an hour, a day, a
     * month or a year per second. */
    int32_t speedPreset;
    bool followNext;
} SosControls;

SosScene *SosSceneCreate(const SosSceneDesc *desc);
void SosSceneDestroy(SosScene *scene);

/* Builds the views of terrain, plants and people on a device, replacing views
 * of an earlier one. The world does not depend on it: a platform that loses
 * its surface, such as Android in the background, detaches the device and
 * attaches a new one later while time goes on. */
bool SosSceneAttachDevice(SosScene *scene, LaiueGraphicsDeviceV2 *device);
/* Releases every GPU resource of the views before the device goes away. */
void SosSceneDetachDevice(SosScene *scene);

/* Applies the controls, advances the world by the elapsed real time and
 * prepares the views. Call before the frame begins: GPU uploads happen here.
 * outViewProjection receives the camera for the frame; false when there is
 * none to set. */
bool SosSceneUpdate(SosScene *scene, const SosControls *controls, double realSeconds,
                    float aspectRatio, float outViewProjection[16]);
/* Submits terrain, plants and people inside the current frame. */
void SosSceneDraw(SosScene *scene);

#endif
