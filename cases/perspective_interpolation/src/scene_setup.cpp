#include "scene_setup.h"

void buildGroundQuadMesh(float halfWidth, float nearZ, float farZ, MeshData& outMesh)
{
    outMesh = MeshData();
    outMesh.vertices = {
        { glm::vec3(-halfWidth, 0.0f, nearZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f) },
        { glm::vec3(halfWidth, 0.0f, nearZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f) },
        { glm::vec3(halfWidth, 0.0f, farZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f) },
        { glm::vec3(-halfWidth, 0.0f, farZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f) },
    };
    outMesh.indices = { 0, 1, 2, 0, 2, 3 };
    outMesh.boundsCenter = glm::vec3(0.0f, 0.0f, 0.5f * (nearZ + farZ));
    outMesh.boundsRadius = glm::length(glm::vec3(halfWidth, 0.0f, 0.5f * (nearZ - farZ)));
}
