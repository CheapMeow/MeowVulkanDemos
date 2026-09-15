#include "scene_setup.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

void buildUvSphereMesh(uint32_t segments, float radius, MeshData& outMesh)
{
    outMesh = MeshData();

    const uint32_t longitudeSegments = std::max(4u, segments);
    const uint32_t latitudeSegments = std::max(2u, segments / 2);
    const float pi = 3.14159265358979323846f;

    for (uint32_t latitude = 0; latitude <= latitudeSegments; ++latitude) {
        const float v = static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        const float theta = v * pi;
        for (uint32_t longitude = 0; longitude <= longitudeSegments; ++longitude) {
            const float u = static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            const float phi = u * 2.0f * pi;

            const glm::vec3 normal(std::sin(theta) * std::cos(phi), std::cos(theta),
                                   std::sin(theta) * std::sin(phi));
            outMesh.vertices.push_back({ normal * radius, normal, glm::vec2(u, v) });
        }
    }

    const uint32_t stride = longitudeSegments + 1;
    for (uint32_t latitude = 0; latitude < latitudeSegments; ++latitude) {
        for (uint32_t longitude = 0; longitude < longitudeSegments; ++longitude) {
            const uint32_t topLeft = latitude * stride + longitude;
            const uint32_t topRight = topLeft + 1;
            const uint32_t bottomLeft = topLeft + stride;
            const uint32_t bottomRight = bottomLeft + 1;

            // 两极那一圈的两个顶点重合，靠近极点的三角形会退化成一条线，跳过
            if (latitude > 0) {
                outMesh.indices.push_back(topLeft);
                outMesh.indices.push_back(topRight);
                outMesh.indices.push_back(bottomRight);
            }
            if (latitude + 1 < latitudeSegments) {
                outMesh.indices.push_back(topLeft);
                outMesh.indices.push_back(bottomRight);
                outMesh.indices.push_back(bottomLeft);
            }
        }
    }

    outMesh.boundsCenter = glm::vec3(0.0f);
    outMesh.boundsRadius = radius;
}
