#pragma once

#include "obj_loader.h"

// 一块面向相机、边长为一的四边形，尺寸由实例里的半边长决定
void buildFacingQuadMesh(MeshData& outMesh);
