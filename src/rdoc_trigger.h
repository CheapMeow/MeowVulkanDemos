#pragma once

// 触发一次 RenderDoc 截帧。RenderDoc 没有注入本进程时为空操作：
// 桌面端注入 renderdoc.dll，安卓端注入的是 VK_LAYER_RENDERDOC_Capture 对应的
// libVkLayer_GLES_RenderDoc.so，两者都只在带 RenderDoc 层启动时存在
void renderdocTriggerCapture();
