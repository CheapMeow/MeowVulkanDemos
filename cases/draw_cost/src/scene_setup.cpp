#include "scene_setup.h"

#include <glm/glm.hpp>

void buildFacingQuadMesh(MeshData& outMesh)
{
    outMesh = MeshData();
    outMesh.vertices = {
        { glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 0.0f) },
        { glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 0.0f) },
        { glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 1.0f) },
        { glm::vec3(-1.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 1.0f) },
    };
    outMesh.indices = { 0, 1, 2, 0, 2, 3 };
    outMesh.boundsCenter = glm::vec3(0.0f);
    outMesh.boundsRadius = 1.41421356f;
}
